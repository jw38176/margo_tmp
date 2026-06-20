#include "pumpkin.h"

#include <cassert>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>

#include "cache.h"
#include "sub_prefetcher_config.hh"

namespace
{

// Convert to cache
champsim::address line_of(champsim::address addr) { return champsim::address{champsim::block_number{addr}}; }

// Mirror the deprecated current_cycle() 
uint64_t cycle_of(const CACHE* cache)
{
  return static_cast<uint64_t>(cache->current_time.time_since_epoch() / cache->clock_period);
}

} // namespace

// Initialize wrappers for sub-prefetchers and controller
pumpkin::pumpkin(CACHE* cache) : prefetcher(cache)
{
  for (const auto& sub_prefetcher : pumpkin_wrappers::SUB_PREFETCHERS) {
    _wrapped_prefetchers.emplace_back(sub_prefetcher.make(cache));
  }
  controller = std::make_unique<Controller>(_wrapped_prefetchers.size(), pumpkin_params::FILTER_TABLE_SIZE);
  _pf_stats.assign(_wrapped_prefetchers.size(), {});
}

const char* pumpkin::sub_prefetcher_name(std::size_t prefetcher_id)
{
  return pumpkin_wrappers::sub_prefetcher_name(prefetcher_id);
}

uint64_t pumpkin::rand_u64()
{
  uint64_t x = _rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  _rng_state = x;
  return x;
}


// Filter prefetch request candidates by sub-prefetchers through the controller, record in pf_sampler, and issue unfiltred prefetches.
void pumpkin::process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, uint64_t current_cycle, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {
    const bool should_pf = controller->should_prefetch(candidate.trigger_ip, prefetcher_id);
    bool will_pf = should_pf;
    if (!should_pf && (rand_u64() % pumpkin_params::NEGATIVE_LEAK_INV_PROB) == 0) {
      will_pf = true;
    }

    const auto pf_line_addr = line_of(candidate.address);

    // PUMPKIN: Record prefetch in pf_sampler
    if (pf_sampler.should_sample(pf_line_addr)) {
      if (auto cap_evt = pf_sampler.record_prefetch(pf_line_addr, current_cycle, candidate.trigger_ip, prefetcher_id, will_pf);
          cap_evt.has_value()) {
        controller->process_eviction_event(*cap_evt, current_cycle);
      }
    }

    // ISSUE PREFETCH
    bool issued = false;
    if (will_pf) {
      issued = prefetch_line(candidate.address, candidate.fill_this_level, candidate.metadata);
    }

    // SUB-PREFETCHER HANDOFF: report whether Pumpkin actually issued it.
    if (candidate.notify_result) {
      candidate.notify_result(issued, !will_pf);
    }

    auto& stats = _pf_stats.at(prefetcher_id);
    stats.candidates++;
    if (!will_pf) {
      stats.filtered++;
    } else if (issued) {
      stats.issued++;
    } else {
      stats.not_issued++;
    }
  }
}

////////////////////////////////////////////////////////////
// PREFETCHER INITIALIZE
////////////////////////////////////////////////////////////

void pumpkin::prefetcher_initialize()
{
  // Initialize pf_sampler with number of prefetchers
  pf_sampler.init(_wrapped_prefetchers.size());
  for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
    wrapped_prefetcher->initialize();
  }
}

////////////////////////////////////////////////////////////
// PREFETCHER CYCLE OPERATE
////////////////////////////////////////////////////////////

void pumpkin::prefetcher_cycle_operate()
{

  const uint64_t current_cycle = cycle_of(intern_);

  // SUB-PREFETCHER HANDOFF
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto candidates = _wrapped_prefetchers[prefetcher_id]->cycle_operate();
    process_candidates(candidates, current_cycle, prefetcher_id);
  }

}


////////////////////////////////////////////////////////////
// PREFETCHER CACHE OPERATE
////////////////////////////////////////////////////////////

uint32_t pumpkin::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                          uint32_t metadata_in)
{
  const uint64_t current_cycle = cycle_of(intern_);
  const champsim::address current_line_addr = line_of(addr);

  // PUMPKIN: Positive training for demand access 
  if (auto access_evt = pf_sampler.notify_demand_access(current_line_addr, current_cycle); access_evt.has_value()) {
    assert(controller != nullptr);
    controller->process_eviction_event(*access_evt, current_cycle);
  }

  uint32_t metadata = metadata_in;
  assert(controller != nullptr);

// SUB-PREFETCHER HANDOFF
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, current_cycle, prefetcher_id);
  }

  return metadata;
}

////////////////////////////////////////////////////////////
// PREFETCHER CACHE FILL
////////////////////////////////////////////////////////////

uint32_t pumpkin::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                       uint32_t metadata_in)
{
  const uint64_t cycle = cycle_of(intern_);

  // PUMPKIN: Notify cache fill of a prefetched line
  if (prefetch) {
#if PUMPKIN_DELAYED_LATE_PREFETCHES
    if (auto fill_evt = pf_sampler.notify_cache_fill(line_of(addr), cycle); fill_evt.has_value()) {
      assert(controller != nullptr);
      controller->process_eviction_event(*fill_evt, fill_evt->entry.demand_timestamp);
    }
#else
    pf_sampler.notify_cache_fill(line_of(addr), cycle);
#endif
  }

  // PUMPKIN: Notify cache eviction to pf_sampler 
  if (evicted_addr != champsim::address{}) {
    if (auto evict_evt = pf_sampler.notify_cache_evict(line_of(evicted_addr)); evict_evt.has_value()) {
      assert(controller != nullptr);
      controller->process_eviction_event(*evict_evt, cycle);
    }
  }

  uint32_t metadata = metadata_in;
  assert(controller != nullptr);

  // SUB-PREFETCHER HANDOFF
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_fill(addr, set, way, prefetch, evicted_addr, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, cycle, prefetcher_id);
  }

  return metadata;
}

////////////////////////////////////////////////////////////
// PREFETCHER FINAL STATS
////////////////////////////////////////////////////////////

void pumpkin::prefetcher_final_stats()
{
  const auto pct = [](uint64_t num, uint64_t denom) -> double {
    return denom == 0 ? 0.0 : (static_cast<double>(num) * 100.0) / static_cast<double>(denom);
  };

  const uint64_t total_candidates =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.candidates; });
  const uint64_t total_filtered =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.filtered; });
  const uint64_t total_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.issued; });
  const uint64_t total_not_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.not_issued; });

  std::cout << "================================================" << std::endl;
  std::cout << "PUMPKIN Prefetch Candidate Stats" << std::endl;
  std::cout << "  Total candidates: " << total_candidates << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Issued:   " << total_issued << " (" << pct(total_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Filtered: " << total_filtered << " (" << pct(total_filtered, total_candidates) << "%)" << std::endl;
  std::cout << "  Selected but not issued: " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)"
            << std::endl;

  std::cout << std::endl << "  Candidate source breakdown:" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _pf_stats.size(); ++prefetcher_id) {
    const auto& stats = _pf_stats[prefetcher_id];
    std::cout << "    " << sub_prefetcher_name(prefetcher_id) << ": " << stats.candidates << " ("
              << pct(stats.candidates, total_candidates) << "%)" << std::endl;
  }

  std::cout << std::endl << "  Issued breakdown by source:" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _pf_stats.size(); ++prefetcher_id) {
    const auto& stats = _pf_stats[prefetcher_id];
    std::cout << "    " << sub_prefetcher_name(prefetcher_id) << ": " << stats.issued << " ("
              << pct(stats.issued, total_issued) << "% of issued, " << pct(stats.issued, total_candidates)
              << "% of candidates)" << std::endl;
  }

  std::cout << std::endl << "  Filtered breakdown by source:" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _pf_stats.size(); ++prefetcher_id) {
    const auto& stats = _pf_stats[prefetcher_id];
    std::cout << "    " << sub_prefetcher_name(prefetcher_id) << ": " << stats.filtered << " ("
              << pct(stats.filtered, total_filtered) << "% of filtered, " << pct(stats.filtered, total_candidates)
              << "% of candidates)" << std::endl;
  }
  std::cout << "================================================" << std::endl;
}
