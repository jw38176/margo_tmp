#include "pumpkin.h"

#include <cassert>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>

#include "../sub_prefetcher/sub_prefetcher_config.hh"
#include "cache.h"
#include "criticality_rob_head_bridge.h"

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
  controller = std::make_unique<Controller>(
      _wrapped_prefetchers.size(), pumpkin_params::FILTER_TABLE_SIZE, [](std::size_t prefetcher_id) { return sub_prefetcher_name(prefetcher_id); });
  _pf_stats.assign(_wrapped_prefetchers.size(), {});
}

const char* pumpkin::sub_prefetcher_name(std::size_t prefetcher_id)
{
  return pumpkin_wrappers::sub_prefetcher_name(prefetcher_id);
}

void pumpkin::set_llc_reference(CACHE* llc)
{
  if (controller != nullptr)
    controller->set_llc(llc);
}

// Filter prefetch request candidates by sub-prefetchers through the controller, record in pf_sampler, and issue unfiltred prefetches.
void pumpkin::process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, uint64_t current_cycle, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {

    // PUMPKIN: Filter prefetch request candidates by sub-prefetchers through the controller
    const bool should_pf = controller->should_prefetch(candidate.trigger_ip, prefetcher_id);

    // PUMPKIN: Leak filtered prefetches
    bool will_pf = should_pf;
    if (!should_pf && (rand_u64() % pumpkin_params::NEGATIVE_LEAK_INV_PROB) == 0) {
      will_pf = true;
    }

    // PUMPKIN: Get prefetch address
    const auto pf_line_addr = line_of(candidate.address);

    // PUMPKIN: Skip Redundant Prefetches
    const bool in_cache = intern_->in_cache(pf_line_addr);

    // PUMPKIN: Check if prefetch is in MSHR
    const bool in_mshr = intern_->in_mshr(pf_line_addr);

    // PUMPKIN: Record prefetch in pf_sampler
    if (!in_cache && pf_sampler.should_sample(pf_line_addr))
    {
      if (auto cap_evt = pf_sampler.record_prefetch(pf_line_addr, current_cycle, candidate.trigger_ip, prefetcher_id, will_pf);
          cap_evt.has_value()) {
        controller->process_eviction_event(*cap_evt, current_cycle);
      }
    }

    // PUMPKIN: Record prefetch in crit_sampler
    if (crit_sampler.should_sample(pf_line_addr) && will_pf && !in_cache && !in_mshr)
    {
      crit_sampler.record_prefetch(pf_line_addr, current_cycle);
    }

    // ISSUE PREFETCH
    const bool redundant = in_cache || in_mshr;
    bool issued = false;
    if (will_pf && !redundant)
    {
      issued = prefetch_line(candidate.address, candidate.fill_this_level, candidate.metadata);
    }

    // SUB-PREFETCHER HANDOFF: report whether Pumpkin actually issued it.
    if (candidate.notify_result) {
      candidate.notify_result(issued, !will_pf); // TODO: Check MLOP's behaviour with this flag (whether to mark redundant prefetches as prefetched)
    }

    auto& stats = _pf_stats.at(prefetcher_id);
    stats.candidates++;
    if (!will_pf) {
      stats.filtered++;
    } else if (redundant) {
      stats.redundant++;
    } else if (issued) {
      stats.issued++;
    } else {
      stats.not_issued++;
    }
  }
}

// PUMPKIN: Process a tracked instruction reaching the head of the ROB
void pumpkin::on_rob_head(uint64_t instr_id, uint64_t cycle)
{
  if (auto crit_evt = crit_sampler.notify_rob_head(instr_id, cycle); crit_evt.has_value())
  {
    assert(controller != nullptr);
    controller->process_criticality_event(*crit_evt);
  }
}

////////////////////////////////////////////////////////////
// PREFETCHER INITIALIZE
////////////////////////////////////////////////////////////

void pumpkin::prefetcher_initialize()
{
  // Initialize pf_sampler with number of prefetchers
  pf_sampler.init(_wrapped_prefetchers.size());
  crit_sampler.init();

  // Register to receive ROB-head notifications for this core.
  champsim::criticality_rob_head_bridge::register_rob_head_sink(intern_->cpu, [this](uint64_t instr_id, uint64_t cycle) { on_rob_head(instr_id, cycle); });

  for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
    wrapped_prefetcher->initialize();
  }

  // Gate Triangel's training on the controller's Critical(Y) * (1 - CU(Y)) decision.
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id)
  {
    if (std::strcmp(sub_prefetcher_name(prefetcher_id), "triangel") == 0)
    {
      _wrapped_prefetchers[prefetcher_id]->set_training_gate([this](champsim::address ip, bool is_miss)
                                                             { return controller->should_track_with_triangel(ip, is_miss); });
    }
  }

  std::cout << "PUMPKIN controller: " << _wrapped_prefetchers.size() << " sub-prefetchers" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id)
  {
    std::cout << "    [" << prefetcher_id << "] " << sub_prefetcher_name(prefetcher_id) << std::endl;
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
  if (auto access_evt = pf_sampler.notify_demand_access(current_line_addr, current_cycle, ip); access_evt.has_value())
  {
    assert(controller != nullptr);
    controller->process_eviction_event(*access_evt, current_cycle);
  }

  // PUMPKIN: Criticality tracking for demand loads. instr_id is supplied by the
  // cache via a side-channel set immediately before this call.
  if (type == access_type::LOAD && crit_sampler.should_sample(current_line_addr))
  {
    crit_sampler.notify_demand_access(current_line_addr, current_cycle, ip, intern_->pref_trigger_instr_id, cache_hit != 0);
  }

  uint32_t metadata = metadata_in;
  assert(controller != nullptr);

  // SUB-PREFETCHER HANDOFF: invoke in fixed ID order (metadata chaining preserved),
  // then process candidates in descending absolute-accuracy rank at the demand PC.
  const std::size_t num_prefetchers = _wrapped_prefetchers.size();
  std::vector<pumpkin_wrappers::WrappedPrefetchResult> results(num_prefetchers);
  for (std::size_t prefetcher_id = 0; prefetcher_id < num_prefetchers; ++prefetcher_id)
  {
    results[prefetcher_id] = _wrapped_prefetchers[prefetcher_id]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = results[prefetcher_id].metadata;
  }

  for (std::size_t prefetcher_id : controller->rank_prefetchers_by_absolute_accuracy(ip))
  {
    process_candidates(results[prefetcher_id].prefetches, current_cycle, prefetcher_id);
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

  if (prefetch)
  {
    // last_fill_from_dram() reflects the line just filled by handle_fill (set
    // immediately before this callback), i.e. the ground-truth origin of `addr`.
    if (auto fill_evt = pf_sampler.notify_cache_fill(line_of(addr), cycle, intern_->last_fill_from_dram()); fill_evt.has_value())
    {
      assert(controller != nullptr);
      controller->process_eviction_event(*fill_evt, fill_evt->entry.demand_timestamp);
    }
  }

  // PUMPKIN: Criticality tracking observes every fill (demand or prefetch).
  crit_sampler.notify_cache_fill(line_of(addr), cycle);

  // PUMPKIN: Notify cache eviction to pf_sampler 
  if (evicted_addr != champsim::address{}) {
    if (auto evict_evt = pf_sampler.notify_cache_evict(line_of(evicted_addr)); evict_evt.has_value()) {
      assert(controller != nullptr);
      controller->process_eviction_event(*evict_evt, cycle);
    }

    // PUMPKIN: Tune criticality sampling rate
    crit_sampler.notify_cache_evict(line_of(evicted_addr));
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
  champsim::criticality_rob_head_bridge::unregister_rob_head_sink(intern_->cpu);

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
  const uint64_t total_redundant =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.redundant; });
  const uint64_t total_not_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.not_issued; });

  std::cout << "================================================" << std::endl;
  std::cout << "PUMPKIN Prefetch Candidate Stats" << std::endl;
  std::cout << "  Total candidates: " << total_candidates << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Issued:   " << total_issued << " (" << pct(total_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Filtered: " << total_filtered << " (" << pct(total_filtered, total_candidates) << "%)" << std::endl;
  std::cout << "  Redundant (in cache/MSHR): " << total_redundant << " (" << pct(total_redundant, total_candidates) << "%)"
            << std::endl;
  std::cout << "  Selected but not issued (PQ full): " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)"
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
  const uint64_t total_critical_loads = controller->critical_loads_recorded();
  const uint64_t total_non_critical_loads = controller->non_critical_loads_recorded();
  const uint64_t total_recorded_loads = total_critical_loads + total_non_critical_loads;

  std::cout << std::endl << "PUMPKIN Criticality Load Stats" << std::endl;
  std::cout << "  Total recorded loads: " << total_recorded_loads << std::endl;
  std::cout << "  Critical:     " << total_critical_loads << " (" << pct(total_critical_loads, total_recorded_loads) << "%)" << std::endl;
  std::cout << "  Non-critical: " << total_non_critical_loads << " (" << pct(total_non_critical_loads, total_recorded_loads) << "%)" << std::endl;

  // Sampler sizing. Each sampler owns an adaptive gate that only sparsens (raises
  // the train ratio above its floor) when its fixed-size table evicts live
  // entries faster than the cache evicts lines, i.e. when the table cannot keep
  // up at the current sampling density. A ratio that never leaves the floor means
  // the table is large enough (with headroom) for this workload; one driven
  // upward means the working set outgrows the table at full density. The prefetch
  // and criticality samplers tune independently from their own eviction events.
  const auto print_sampler_sizing = [](const char* name, const Sampler::SamplerStats& s)
  {
    const char* sizing_verdict;
    if (s.peak_train_ratio <= s.min_train_ratio)
      sizing_verdict = "yes - gate stayed at its floor, table has headroom at max density";
    else if (s.train_ratio <= s.min_train_ratio)
      sizing_verdict = "borderline - transient pressure, but settled back at the floor";
    else
      sizing_verdict = "no - sustained table pressure, running below max density";

    std::cout << std::endl << "PUMPKIN " << name << " Sizing Stats" << std::endl;
    std::cout << "  Final train ratio:  1-in-" << s.train_ratio << " (floor 1-in-" << s.min_train_ratio << ", ceiling 1-in-" << s.max_train_ratio << ")"
              << std::endl;
    std::cout << "  Peak train ratio:   1-in-" << s.peak_train_ratio << std::endl;
    std::cout << "  Retunes:            " << s.retune_up << " sparser (table pressure), " << s.retune_down << " denser (table headroom)" << std::endl;
    std::cout << "  Calibration evicts: " << s.table_evicts_total << " sampler-table, " << s.cache_evicts_total << " cache" << std::endl;
    std::cout << "  Large enough?       " << sizing_verdict << std::endl;
  };

  print_sampler_sizing("Prefetch Sampler", pf_sampler.stats());
  print_sampler_sizing("Criticality Sampler", crit_sampler.stats());
  std::cout << "  ROB heads w/o fill: " << crit_sampler.rob_heads_without_fill() << std::endl;

  const uint64_t cache_hit_events = pf_sampler.cache_hit_events();
  const uint64_t late_prefetch_events = pf_sampler.late_prefetch_events();
  const uint64_t positive_terminal_events = cache_hit_events + late_prefetch_events;

  std::cout << std::endl << "PUMPKIN Prefetch Sampler Terminal Event Stats" << std::endl;
  std::cout << "  CacheHit:     " << cache_hit_events << " (" << pct(cache_hit_events, positive_terminal_events) << "%)" << std::endl;
  std::cout << "  LatePrefetch: " << late_prefetch_events << " (" << pct(late_prefetch_events, positive_terminal_events) << "%)" << std::endl;
  if (cache_hit_events == 0)
    std::cout << "  LatePrefetch/CacheHit ratio: n/a (no CacheHit events)" << std::endl;
  else
    std::cout << "  LatePrefetch/CacheHit ratio: " << std::fixed << std::setprecision(4)
              << (static_cast<double>(late_prefetch_events) / static_cast<double>(cache_hit_events)) << std::endl;

  const uint64_t inacc_dram_fill0 = controller->inaccurate_dram_fill_zero();
  const uint64_t inacc_dram_filln = controller->inaccurate_dram_fill_nonzero();
  const uint64_t inacc_llc_fill0 = controller->inaccurate_llc_fill_zero();
  const uint64_t inacc_llc_filln = controller->inaccurate_llc_fill_nonzero();
  const uint64_t inacc_dram = inacc_dram_fill0 + inacc_dram_filln;
  const uint64_t inacc_llc = inacc_llc_fill0 + inacc_llc_filln;
  const uint64_t inacc_total = inacc_dram + inacc_llc;

  std::cout << std::endl << "PUMPKIN Inaccurate Prefetch Origin Stats (negative path)" << std::endl;
  std::cout << "  Total inaccurate: " << inacc_total << std::endl;
  std::cout << "  From DRAM: " << inacc_dram << " (" << pct(inacc_dram, inacc_total) << "%)" << std::endl;
  std::cout << "    fill_latency == 0: " << inacc_dram_fill0 << " (" << pct(inacc_dram_fill0, inacc_dram) << "% of DRAM)" << std::endl;
  std::cout << "    fill_latency != 0: " << inacc_dram_filln << " (" << pct(inacc_dram_filln, inacc_dram) << "% of DRAM)" << std::endl;
  std::cout << "  From LLC:  " << inacc_llc << " (" << pct(inacc_llc, inacc_total) << "%)" << std::endl;
  std::cout << "    fill_latency == 0: " << inacc_llc_fill0 << " (" << pct(inacc_llc_fill0, inacc_llc) << "% of LLC)" << std::endl;
  std::cout << "    fill_latency != 0: " << inacc_llc_filln << " (" << pct(inacc_llc_filln, inacc_llc) << "% of LLC)" << std::endl;

  controller->print_filter_table_stats(std::cout, [](std::size_t prefetcher_id) { return sub_prefetcher_name(prefetcher_id); });
  controller->print_coverage_filter_stats(std::cout);

  std::cout << "================================================" << std::endl;
}
