#include "ipop.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>

#include "../pumpkin/sub_prefetcher_config.hh"
#include "cache.h"

// ───────────────────────────────────────────────────────────────────────────────
// IPOP degree controller.
//
// Sub-prefetcher set: identical to Pumpkin's (pumpkin_wrappers::SUB_PREFETCHERS).
//
// Adaptation notes vs. the original (legacy-ChampSim) IPOP/UDC implementation:
//   * The legacy version split useful/pollution events by fill source (LLC vs DRAM) and weighted them
//     by measured access latency, and computed a bus/bank "delay effect". The modular ChampSim cache
//     model does not expose DRAM latency, row-buffer state, or per-request DRAM bookkeeping to a
//     prefetcher module, so the net effect here is the unweighted (useful - pollution) count.
//   * The legacy version set each sub-prefetcher's internal degree via set_degree(). The wrapped
//     sub-prefetchers do not expose that, so the degree is enforced as a per-request issue budget:
//     prefetcher i may issue at most current_state[i] prefetches per demand request (and none when
//     blocked).
//   * The DRAM-latency congestion signal is replaced by the cache's average MSHR occupancy ratio.
// ───────────────────────────────────────────────────────────────────────────────

namespace
{
uint64_t block_of(champsim::address addr) { return addr.to<uint64_t>() >> LOG2_BLOCK_SIZE; }
} // namespace

ipop::ipop(CACHE* cache) : prefetcher(cache)
{
  for (const auto& sub_prefetcher : pumpkin_wrappers::SUB_PREFETCHERS) {
    _wrapped_prefetchers.emplace_back(sub_prefetcher.make(cache));
  }

  const std::size_t n = _wrapped_prefetchers.size();
  _current_state.assign(n, ipop_params::MID_DEGREE);
  _issue_budget.assign(n, ipop_params::MID_DEGREE);
  _useful_count.assign(n, 0);
  _pollution_count.assign(n, 0);
  _fill_count.assign(n, 0);
  _pf_stats.assign(n, {});
}

const char* ipop::sub_prefetcher_name(std::size_t prefetcher_id) { return pumpkin_wrappers::sub_prefetcher_name(prefetcher_id); }

// ─── Tagged-table index/tag helpers ─────────────────────────────────────────────

std::size_t ipop::pf_index(uint64_t block)
{
  const uint64_t hashed = block ^ (block >> ipop_params::PF_ADDR_TABLE_INDEX_BITS) ^ (block >> (2 * ipop_params::PF_ADDR_TABLE_INDEX_BITS));
  return static_cast<std::size_t>(hashed & (ipop_params::PF_ADDR_TABLE_SIZE - 1));
}

uint64_t ipop::pf_tag(uint64_t block)
{
  return (block >> ipop_params::PF_ADDR_TABLE_INDEX_BITS) & ((1ULL << ipop_params::PF_ADDR_TABLE_TAG_BITS) - 1);
}

std::size_t ipop::poll_index(uint64_t block)
{
  const uint64_t hashed = block ^ (block >> ipop_params::POLL_ADDR_TABLE_INDEX_BITS) ^ (block >> (2 * ipop_params::POLL_ADDR_TABLE_INDEX_BITS));
  return static_cast<std::size_t>(hashed & (ipop_params::POLL_ADDR_TABLE_SIZE - 1));
}

uint64_t ipop::poll_tag(uint64_t block)
{
  return (block >> ipop_params::POLL_ADDR_TABLE_INDEX_BITS) & ((1ULL << ipop_params::POLL_ADDR_TABLE_TAG_BITS) - 1);
}

// ─── Degree gate ────────────────────────────────────────────────────────────────

// Replenish every sub-prefetcher's per-request budget to its current degree. Called at each demand
// access; the budget then bounds how many prefetches that prefetcher can issue while servicing the
// request (including lookahead continuations that arrive on later cycles).
void ipop::replenish_budgets()
{
  for (std::size_t i = 0; i < _issue_budget.size(); ++i) {
    _issue_budget[i] = std::max(_current_state[i], 0);
  }
}

// ─── Event recording ────────────────────────────────────────────────────────────

void ipop::record_prefetch_issue(champsim::address pf_addr, std::size_t prefetcher_id)
{
  const uint64_t block = block_of(pf_addr);
  auto& entry = _pf_table[pf_index(block)];
  entry.valid = true;
  entry.tag = pf_tag(block);
  entry.prefetcher_id = prefetcher_id;
}

void ipop::note_useful(champsim::address demand_addr)
{
  const uint64_t block = block_of(demand_addr);
  auto& entry = _pf_table[pf_index(block)];
  if (entry.valid && entry.tag == pf_tag(block)) {
    _useful_count[entry.prefetcher_id]++;
    _pf_stats[entry.prefetcher_id].useful++;
    entry.valid = false; // credit a prefetched line only once
  }
}

void ipop::note_pollution_miss(champsim::address demand_addr)
{
  const uint64_t block = block_of(demand_addr);
  auto& entry = _poll_table[poll_index(block)];
  if (entry.valid && entry.tag == poll_tag(block)) {
    _pollution_count[entry.prefetcher_id]++;
    _pf_stats[entry.prefetcher_id].pollution++;
    entry.valid = false;
  }
}

// ─── Phase control ──────────────────────────────────────────────────────────────

void ipop::run_control()
{
  const std::size_t n = _wrapped_prefetchers.size();
  const double congestion = (_congestion_samples > 0) ? (_congestion_sum / static_cast<double>(_congestion_samples)) : 0.0;

  std::vector<int64_t> effect(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    effect[i] = static_cast<int64_t>(_useful_count[i]) - static_cast<int64_t>(_pollution_count[i]);
  }

  // Find the most-useful active prefetcher (to promote) and the least-useful active one (to demote).
  int64_t max_effect = std::numeric_limits<int64_t>::min();
  int64_t min_effect = std::numeric_limits<int64_t>::max();
  int max_index = -1;
  int min_index = -1;
  for (std::size_t i = 0; i < n; ++i) {
    if (_current_state[i] <= 0) {
      continue;
    }
    if (_current_state[i] < ipop_params::HIGH_DEGREE && effect[i] > max_effect) {
      max_effect = effect[i];
      max_index = static_cast<int>(i);
    }
    if (_current_state[i] > ipop_params::LOW_DEGREE && effect[i] < min_effect) {
      min_effect = effect[i];
      min_index = static_cast<int>(i);
    }
  }

  // Congestion-driven single-step adjustment: relieve pressure by demoting the worst, or spend slack
  // by promoting the best.
  if (congestion > ipop_params::CONGESTION_HIGH && min_index >= 0) {
    _current_state[min_index] -= ipop_params::DEGREE_STEP;
  } else if (congestion < ipop_params::CONGESTION_LOW && max_index >= 0) {
    _current_state[max_index] += ipop_params::DEGREE_STEP;
  }

  // Per-prefetcher state machine.
  for (std::size_t i = 0; i < n; ++i) {
    if (_current_state[i] < 0) {
      _current_state[i]++; // unblock countdown
    } else if (_current_state[i] == 0) {
      if (congestion < ipop_params::CONGESTION_LOW) {
        _current_state[i] = ipop_params::LOW_DEGREE; // reactivate during a calm phase
      }
    } else {
      if (effect[i] < 0) {
        _current_state[i] = -ipop_params::BLOCK_PHASE; // net-harmful: block for BLOCK_PHASE phases
      } else {
        _current_state[i] = std::clamp(_current_state[i], ipop_params::LOW_DEGREE, ipop_params::HIGH_DEGREE);
      }
    }
  }

  _phase_count++;
  reset_phase_counters();
}

void ipop::reset_phase_counters()
{
  std::fill(_useful_count.begin(), _useful_count.end(), 0);
  std::fill(_pollution_count.begin(), _pollution_count.end(), 0);
  std::fill(_fill_count.begin(), _fill_count.end(), 0);
  _demand_count = 0;
  _congestion_sum = 0.0;
  _congestion_samples = 0;
}

// ─── Candidate handling ─────────────────────────────────────────────────────────

void ipop::process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {
    // Degree limit: only let this prefetcher produce more prefetches for the current request while it
    // still has budget left.
    const bool gate = (_issue_budget[prefetcher_id] > 0);

    bool issued = false;
    if (gate) {
      issued = prefetch_line(candidate.address, candidate.fill_this_level, candidate.metadata);
      if (issued) {
        _issue_budget[prefetcher_id]--;
        record_prefetch_issue(candidate.address, prefetcher_id);
      }
    }

    // Report back to the sub-prefetcher whether IPOP issued the line (and, if not, whether it was
    // throttled rather than refused by the cache) so lookahead state machines stay consistent.
    if (candidate.notify_result) {
      candidate.notify_result(issued, !gate);
    }

    auto& stats = _pf_stats.at(prefetcher_id);
    stats.candidates++;
    if (!gate) {
      stats.throttled++;
    } else if (issued) {
      stats.issued++;
    } else {
      stats.not_issued++;
    }
  }
}

// ─── ChampSim prefetcher hooks ──────────────────────────────────────────────────

void ipop::prefetcher_initialize()
{
  for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
    wrapped_prefetcher->initialize();
  }

  std::cout << "IPOP degree controller: " << _wrapped_prefetchers.size() << " sub-prefetchers, phase-length=" << ipop_params::PHASE_LENGTH
            << ", degree levels [" << ipop_params::LOW_DEGREE << "," << ipop_params::MID_DEGREE << "," << ipop_params::HIGH_DEGREE << "]" << std::endl;
}

void ipop::prefetcher_cycle_operate()
{
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto candidates = _wrapped_prefetchers[prefetcher_id]->cycle_operate();
    process_candidates(candidates, prefetcher_id);
  }
}

uint32_t ipop::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                        uint32_t metadata_in)
{
  const bool is_demand = (type != access_type::PREFETCH);
  if (is_demand) {
    _demand_count++;
    _congestion_sum += intern_->get_mshr_occupancy_ratio();
    _congestion_samples++;

    // Grant each sub-prefetcher a fresh per-request budget equal to its current degree.
    replenish_budgets();

    if (cache_hit) {
      note_useful(addr); // demand hit may be on a line a sub-prefetcher brought in
    } else {
      note_pollution_miss(addr); // demand miss may be on a line a prefetch evicted
    }

    if (_demand_count >= ipop_params::PHASE_LENGTH) {
      run_control();
    }
  }

  uint32_t metadata = metadata_in;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, prefetcher_id);
  }

  return metadata;
}

uint32_t ipop::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  // A prefetched line is being installed. Attribute its eviction victim to the issuing sub-prefetcher
  // so a later demand miss on that victim can be charged back as pollution.
  if (prefetch) {
    const uint64_t block = block_of(addr);
    const auto& filled = _pf_table[pf_index(block)];
    if (filled.valid && filled.tag == pf_tag(block)) {
      const std::size_t issuer = filled.prefetcher_id;
      _fill_count[issuer]++;
      if (evicted_addr != champsim::address{}) {
        const uint64_t evicted_block = block_of(evicted_addr);
        auto& victim = _poll_table[poll_index(evicted_block)];
        victim.valid = true;
        victim.tag = poll_tag(evicted_block);
        victim.prefetcher_id = issuer;
      }
    }
  }

  uint32_t metadata = metadata_in;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_fill(addr, set, way, prefetch, evicted_addr, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, prefetcher_id);
  }

  return metadata;
}

void ipop::prefetcher_final_stats()
{
  const auto pct = [](uint64_t num, uint64_t denom) -> double {
    return denom == 0 ? 0.0 : (static_cast<double>(num) * 100.0) / static_cast<double>(denom);
  };

  const uint64_t total_candidates =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0}, [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.candidates; });
  const uint64_t total_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0}, [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.issued; });
  const uint64_t total_throttled =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0}, [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.throttled; });
  const uint64_t total_not_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0}, [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.not_issued; });

  std::cout << "================================================" << std::endl;
  std::cout << "IPOP Prefetch Candidate Stats" << std::endl;
  std::cout << "  Control phases: " << _phase_count << std::endl;
  std::cout << "  Total candidates: " << total_candidates << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Issued:    " << total_issued << " (" << pct(total_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Throttled: " << total_throttled << " (" << pct(total_throttled, total_candidates) << "%)" << std::endl;
  std::cout << "  Selected but not issued: " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)" << std::endl;

  std::cout << std::endl << "  Per sub-prefetcher (final-degree | candidates | issued | useful | pollution):" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _pf_stats.size(); ++prefetcher_id) {
    const auto& stats = _pf_stats[prefetcher_id];
    std::cout << "    " << sub_prefetcher_name(prefetcher_id) << ": degree=" << _current_state[prefetcher_id] << " | cand=" << stats.candidates
              << " | issued=" << stats.issued << " (" << pct(stats.issued, total_issued) << "% of issued)"
              << " | useful=" << stats.useful << " | pollution=" << stats.pollution << std::endl;
  }
  std::cout << "================================================" << std::endl;
}
