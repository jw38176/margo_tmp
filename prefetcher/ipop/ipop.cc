#include "ipop.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>

#include "../sub_prefetcher/sub_prefetcher_config.hh"
#include "cache.h"

// ───────────────────────────────────────────────────────────────────────────────
// IPOP degree controller.
//
// Sub-prefetcher set: identical to Margo's (margo_wrappers::SUB_PREFETCHERS).
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
  for (const auto& sub_prefetcher : margo_wrappers::sub_prefetchers_for(cache->NAME)) {
    _wrapped_prefetchers.emplace_back(sub_prefetcher.make(cache));
    _sub_prefetcher_names.push_back(sub_prefetcher.name);
  }

  const std::size_t n = _wrapped_prefetchers.size();
  _current_state.assign(n, ipop_params::MID_DEGREE);
  _issue_budget.assign(n, ipop_params::MID_DEGREE);
  _useful_l3.assign(n, 0);
  _useful_dram.assign(n, 0);
  _pollution_l3.assign(n, 0);
  _pollution_dram.assign(n, 0);
  _fill_count.assign(n, 0);
  _pf_stats.assign(n, {});
}

const char* ipop::sub_prefetcher_name(std::size_t prefetcher_id) const
{
  return prefetcher_id < _sub_prefetcher_names.size() ? _sub_prefetcher_names[prefetcher_id] : "unknown";
}

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
    if (entry.from_dram) {
      _useful_dram[entry.prefetcher_id]++;
    } else {
      _useful_l3[entry.prefetcher_id]++;
    }
    _pf_stats[entry.prefetcher_id].useful++;
    entry.valid = false; // credit a prefetched line only once
  }
}

void ipop::note_pollution_miss(champsim::address demand_addr, bool from_dram)
{
  const uint64_t block = block_of(demand_addr);
  auto& entry = _poll_table[poll_index(block)];
  if (entry.valid && entry.tag == poll_tag(block)) {
    if (from_dram) {
      _pollution_dram[entry.prefetcher_id]++;
    } else {
      _pollution_l3[entry.prefetcher_id]++;
    }
    _pf_stats[entry.prefetcher_id].pollution++;
    entry.valid = false;
  }
}

// ─── Phase control ──────────────────────────────────────────────────────────────

void ipop::run_control()
{
  const std::size_t n = _wrapped_prefetchers.size();

  // Real measured latencies from the cache, used both to weight each prefetcher's net effect and to
  // gauge DRAM congestion (replacing the earlier MSHR-occupancy proxy).
  const float lat_l3 = intern_->get_l3_latency();
  const float lat_dram = intern_->get_dram_latency();

  // Latency-weighted net effect: benefit of useful prefetches minus cost of pollution, each event
  // charged the latency of the level it touched.
  std::vector<int64_t> effect(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    const float useful_effect = static_cast<float>(_useful_l3[i]) * lat_l3 + static_cast<float>(_useful_dram[i]) * lat_dram;
    const float poll_effect = static_cast<float>(_pollution_l3[i]) * lat_l3 + static_cast<float>(_pollution_dram[i]) * lat_dram;
    effect[i] = static_cast<int64_t>(useful_effect - poll_effect);
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

  // DRAM-latency-driven single-step adjustment: relieve pressure by demoting the worst when DRAM is
  // heavily loaded, or spend slack by promoting the best when DRAM latency is near-ideal.
  const bool dram_congested = lat_dram > ipop_params::IDEAL_DRAM_TIME * ipop_params::DRAM_THROTTLE_FACTOR;
  const bool dram_relaxed = lat_dram < ipop_params::IDEAL_DRAM_TIME * ipop_params::DRAM_PROMOTE_FACTOR;
  if (dram_congested && min_index >= 0) {
    _current_state[min_index] -= ipop_params::DEGREE_STEP;
  } else if (dram_relaxed && max_index >= 0) {
    _current_state[max_index] += ipop_params::DEGREE_STEP;
  }

  // Per-prefetcher state machine.
  for (std::size_t i = 0; i < n; ++i) {
    if (_current_state[i] < 0) {
      _current_state[i]++; // unblock countdown
    } else if (_current_state[i] == 0) {
      if (dram_relaxed) {
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
  intern_->reset_latency_stats();
}

void ipop::reset_phase_counters()
{
  std::fill(_useful_l3.begin(), _useful_l3.end(), 0);
  std::fill(_useful_dram.begin(), _useful_dram.end(), 0);
  std::fill(_pollution_l3.begin(), _pollution_l3.end(), 0);
  std::fill(_pollution_dram.begin(), _pollution_dram.end(), 0);
  std::fill(_fill_count.begin(), _fill_count.end(), 0);
  _demand_count = 0;
}

// ─── Candidate handling ─────────────────────────────────────────────────────────

void ipop::process_candidates(std::vector<margo_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {
    // Degree limit: only let this prefetcher produce more prefetches for the current request while it
    // still has budget left.
    const bool gate = (_issue_budget[prefetcher_id] > 0);

    // Stats-only residency probe (does not gate prefetch_line()).
    const champsim::address pf_line_addr{champsim::block_number{candidate.address}};
    const bool redundant = intern_->in_cache(pf_line_addr) || intern_->in_mshr(pf_line_addr);

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
    } else if (redundant) {
      stats.redundant++;
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
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id)
  {
    std::cout << "    [" << prefetcher_id << "] " << sub_prefetcher_name(prefetcher_id) << std::endl;
  }
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

    // Grant each sub-prefetcher a fresh per-request budget equal to its current degree.
    replenish_budgets();

    if (cache_hit) {
      note_useful(addr); // demand hit may be on a line a sub-prefetcher brought in
    }
    // A demand miss's pollution is attributed at fill time (prefetcher_cache_fill), where the refill's
    // source latency (LLC vs DRAM) is known.

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
  const bool from_dram = intern_->last_fill_from_dram();

  if (prefetch) {
    // A prefetched line is being installed. Record its fill source so a later useful hit is charged
    // the right latency, and attribute its eviction victim to the issuing sub-prefetcher so a later
    // demand miss on that victim can be charged back as pollution.
    const uint64_t block = block_of(addr);
    auto& filled = _pf_table[pf_index(block)];
    if (filled.valid && filled.tag == pf_tag(block)) {
      const std::size_t issuer = filled.prefetcher_id;
      filled.from_dram = from_dram;
      _fill_count[issuer]++;
      if (evicted_addr != champsim::address{}) {
        const uint64_t evicted_block = block_of(evicted_addr);
        auto& victim = _poll_table[poll_index(evicted_block)];
        victim.valid = true;
        victim.tag = poll_tag(evicted_block);
        victim.prefetcher_id = issuer;
      }
    }
  } else {
    // A demand line is being refilled. If it was previously evicted by a prefetch, charge that
    // prefetcher pollution at the latency of this refill's source.
    note_pollution_miss(addr, from_dram);
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
  const uint64_t total_redundant =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0}, [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.redundant; });
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
  std::cout << "  Redundant (in cache/MSHR): " << total_redundant << " (" << pct(total_redundant, total_candidates) << "%)" << std::endl;
  std::cout << "  Throttled: " << total_throttled << " (" << pct(total_throttled, total_candidates) << "%)" << std::endl;
  std::cout << "  Selected but not issued: " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)" << std::endl;

  std::cout << std::endl << "  Per sub-prefetcher (final-degree | candidates | issued | redundant | useful | pollution):" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _pf_stats.size(); ++prefetcher_id) {
    const auto& stats = _pf_stats[prefetcher_id];
    std::cout << "    " << sub_prefetcher_name(prefetcher_id) << ": degree=" << _current_state[prefetcher_id] << " | cand=" << stats.candidates
              << " | issued=" << stats.issued << " (" << pct(stats.issued, total_issued) << "% of issued)"
              << " | redundant=" << stats.redundant << " | useful=" << stats.useful << " | pollution=" << stats.pollution << std::endl;
  }
  std::cout << "================================================" << std::endl;
}
