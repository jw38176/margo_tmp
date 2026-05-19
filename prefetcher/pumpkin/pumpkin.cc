#include "pumpkin.h"

#include <cassert>
#include <memory>

#include "cache.h"
#include "../caerus/pumpkin_wrapper.h"
#include "../gaze/pumpkin_wrapper.h"
#include "../ip_stride/pumpkin_wrapper.h"

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

// Initialize wrappers for sub-prefetchers and controller=
pumpkin::pumpkin(CACHE* cache) : prefetcher(cache)
{
  _wrapped_prefetchers.emplace_back(make_ip_stride_pumpkin_wrapper(cache));
  _wrapped_prefetchers.emplace_back(make_caerus_pumpkin_wrapper(cache));
  _wrapped_prefetchers.emplace_back(make_gaze_pumpkin_wrapper(cache));
  _controller = std::make_unique<Controller>(_wrapped_prefetchers.size(), pumpkin_params::FILTER_TABLE_SIZE);
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


// Filter prefetch request candidates by sub-prefetchers through the controller, record in sampler, and issue unfiltred prefetches.
void pumpkin::process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, uint64_t current_cycle, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {
    const bool should_pf = _controller->should_prefetch(candidate.trigger_ip, prefetcher_id);
    bool will_pf = should_pf;
    if (!should_pf && (rand_u64() % pumpkin_params::NEGATIVE_LEAK_INV_PROB) == 0) {
      will_pf = true;
    }

    // PUMPKIN: Record prefetch in sampler
    if (_sampler.should_sample()) {
      if (auto cap_evt = _sampler.record_prefetch(line_of(candidate.address), current_cycle, candidate.trigger_ip, prefetcher_id, will_pf);
          cap_evt.has_value()) {
        _controller->process_eviction_event(*cap_evt, current_cycle);
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
  }
}

void pumpkin::prefetcher_initialize()
{
  // Initialize sampler with number of prefetchers
  _sampler.init(_wrapped_prefetchers.size());
  for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
    wrapped_prefetcher->initialize();
  }
}

void pumpkin::prefetcher_cycle_operate()
{

  const uint64_t current_cycle = cycle_of(intern_);

  // SUB-PREFETCHER HANDOFF
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto candidates = _wrapped_prefetchers[prefetcher_id]->cycle_operate();
    process_candidates(candidates, current_cycle, prefetcher_id);
  }

}

uint32_t pumpkin::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                          uint32_t metadata_in)
{
  const uint64_t current_cycle = cycle_of(intern_);
  const champsim::address current_line_addr = line_of(addr);

  // PUMPKIN: Positive training for demand access 
  if (auto access_evt = _sampler.notify_demand_access(current_line_addr, current_cycle); access_evt.has_value()) {
    assert(_controller != nullptr);
    _controller->process_eviction_event(*access_evt, current_cycle);
  }

  uint32_t metadata = metadata_in;
  assert(_controller != nullptr);

// SUB-PREFETCHER HANDOFF
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, current_cycle, prefetcher_id);
  }

  return metadata;
}

uint32_t pumpkin::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                       uint32_t metadata_in)
{
  const uint64_t cycle = cycle_of(intern_);

  // PUMPKIN: Notify cache fill of a prefetched line
  if (prefetch) {
    _sampler.notify_cache_fill(line_of(addr), cycle);
  }

  // PUMPKIN: Notify cache eviction to sampler 
  if (evicted_addr != champsim::address{}) {
    if (auto evict_evt = _sampler.notify_cache_evict(line_of(evicted_addr)); evict_evt.has_value()) {
      assert(_controller != nullptr);
      _controller->process_eviction_event(*evict_evt, cycle);
    }
  }

  uint32_t metadata = metadata_in;
  assert(_controller != nullptr);

  // SUB-PREFETCHER HANDOFF
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_fill(addr, set, way, prefetch, evicted_addr, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, cycle, prefetcher_id);
  }

  return metadata;
}
