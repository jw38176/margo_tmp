#include "alecto.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <numeric>

#include "../sub_prefetcher/sub_prefetcher_config.hh"
#include "cache.h"

namespace
{
uint64_t block_of(champsim::address addr) { return addr.to<uint64_t>() >> LOG2_BLOCK_SIZE; }
} // namespace

alecto::alecto(CACHE* cache) : prefetcher(cache)
{
  for (const auto& sub_prefetcher : margo_wrappers::sub_prefetchers_for(cache->NAME)) {
    _wrapped_prefetchers.emplace_back(sub_prefetcher.make(cache));
    _sub_prefetcher_names.push_back(sub_prefetcher.name);
  }
  _n = _wrapped_prefetchers.size();

  for (auto& entry : _allocation_table) {
    entry.states.assign(_n, 0);
  }
  for (auto& entry : _sample_table) {
    entry.issued.assign(_n, 0);
    entry.confirmed.assign(_n, 0);
  }
  for (auto& entry : _sandbox_table) {
    entry.issued.assign(_n, false);
    entry.trigger.assign(_n, champsim::address{});
  }

  _issue_budget.assign(_n, alecto_params::BASE_DEGREE);
  _demand_allowed.assign(_n, 1);
  _pf_stats.assign(_n, {});
}

const char* alecto::sub_prefetcher_name(std::size_t prefetcher_id) const
{
  return prefetcher_id < _sub_prefetcher_names.size() ? _sub_prefetcher_names[prefetcher_id] : "unknown";
}

// ─── Index / tag helpers ─────────────────────────────────────────────────────────

uint64_t alecto::xor_fold(uint64_t key, int index_len)
{
  if (index_len == 0) {
    return key;
  }
  for (uint64_t tag = (key >> index_len); tag > 0; tag >>= index_len) {
    key ^= tag & ((1ULL << index_len) - 1);
  }
  return key;
}

std::size_t alecto::pc_index(uint64_t pc) { return xor_fold(pc, alecto_params::PC_INDEX_BITS) % alecto_params::ALLOCATION_TABLE_SIZE; }

std::size_t alecto::sample_index(uint64_t pc) { return xor_fold(pc, alecto_params::PC_INDEX_BITS) % alecto_params::SAMPLE_TABLE_SIZE; }

uint16_t alecto::pc_tag(uint64_t pc) { return static_cast<uint16_t>((pc >> alecto_params::PC_INDEX_BITS) & ((1ULL << alecto_params::PC_TAG_BITS) - 1)); }

std::size_t alecto::sandbox_index(uint64_t block) { return xor_fold(block, alecto_params::SANDBOX_INDEX_BITS) % alecto_params::SANDBOX_TABLE_SIZE; }

uint8_t alecto::sandbox_tag(uint64_t block)
{
  return static_cast<uint8_t>((block >> alecto_params::SANDBOX_INDEX_BITS) & ((1ULL << alecto_params::SANDBOX_TAG_BITS) - 1));
}

// ─── Demand path ─────────────────────────────────────────────────────────────────

// Replenish the per-demand issue budget from the demanding PC's learned control state, then credit
// any prefetch this demand confirms as useful. Mirrors the original Access().
void alecto::access(champsim::address ip, champsim::address addr)
{
  const uint64_t pc = ip.to<uint64_t>();
  const std::size_t index = pc_index(pc);
  const uint16_t tag = pc_tag(pc);

  // Default: every sub-prefetcher may issue up to BASE_DEGREE prefetches this demand.
  std::fill(_issue_budget.begin(), _issue_budget.end(), alecto_params::BASE_DEGREE);

  const auto& alloc = _allocation_table[index];
  if (alloc.valid && alloc.tag == tag) {
    for (std::size_t i = 0; i < _n; ++i) {
      if (alloc.states[i] > 0) {
        _issue_budget[i] = alecto_params::BASE_DEGREE + alloc.states[i]; // promote
      } else if (alloc.states[i] < 0) {
        _issue_budget[i] = 0; // blocked
      }
    }
  }

  check_sandbox(addr);
}

// Dynamic demand request allocation (paper §IV-B / §IV-E). Decide which sub-prefetchers this PC's demand
// is routed to: Alecto emits an identifier only for sub-prefetchers in the UI (state 0) or IA (state > 0)
// states, so a sub-prefetcher in the IB (blocked, state < 0) state receives nothing. Withholding the
// request means the blocked sub-prefetcher is neither trained on an out-of-field access nor given a
// chance to generate from it, which is what keeps its table from being polluted. A PC with no allocation
// entry defaults to "all allowed" (every sub-prefetcher starts in UI).
void alecto::compute_allocation(champsim::address ip)
{
  std::fill(_demand_allowed.begin(), _demand_allowed.end(), static_cast<char>(1));

  const uint64_t pc = ip.to<uint64_t>();
  const auto& alloc = _allocation_table[pc_index(pc)];
  if (alloc.valid && alloc.tag == pc_tag(pc)) {
    for (std::size_t i = 0; i < _n; ++i) {
      _demand_allowed[i] = static_cast<char>(alloc.states[i] >= 0);
    }
  }
}

// A demand that lands on a recently prefetched block confirms the issuing sub-prefetcher(s).
void alecto::check_sandbox(champsim::address addr)
{
  const uint64_t block = block_of(addr);
  const std::size_t index = sandbox_index(block);
  const uint8_t tag = sandbox_tag(block);

  auto& entry = _sandbox_table[index];
  if (entry.valid && entry.tag == tag) {
    for (std::size_t i = 0; i < _n; ++i) {
      if (entry.issued[i]) {
        confirm(entry.trigger[i], i);
      }
    }
    // Credit a prefetched line only once.
    entry.valid = false;
    std::fill(entry.issued.begin(), entry.issued.end(), false);
  }
}

// Record one confirmed (useful) prefetch against the PC that issued it. Mirrors SampleTableIncrease().
void alecto::confirm(champsim::address ip, std::size_t prefetcher_id)
{
  const uint64_t pc = ip.to<uint64_t>();
  const std::size_t index = sample_index(pc);
  const uint16_t tag = pc_tag(pc);

  auto& sample = _sample_table[index];
  if (sample.valid && sample.tag == tag) {
    sample.confirmed[prefetcher_id]++;
    _pf_stats[prefetcher_id].useful++;
  }
}

// Per-demand sample-table bookkeeping for a tracked PC: advance the phase counter, roll the phase over
// at PERIOD, and track consecutive "dead" demands. Mirrors the sample-table half of IssuePrefetch().
void alecto::account_demand(champsim::address ip)
{
  const uint64_t pc = ip.to<uint64_t>();
  const std::size_t index = sample_index(pc);
  const uint16_t tag = pc_tag(pc);

  auto& sample = _sample_table[index];

  // Only tracked PCs (entries created once a prefetch was attributed to them) accumulate demands. An
  // untracked PC is, by definition, one that has never produced a prefetch: nothing to learn yet.
  if (!(sample.valid && sample.tag == tag)) {
    return;
  }

  // Liveness: did this PC issue any prefetch since its previous demand? If not, it is "dead" this round.
  const bool dead = (sample.issued_since_demand == 0);
  sample.issued_since_demand = 0;

  sample.demands++;
  if (sample.demands >= alecto_params::PERIOD) {
    end_of_phase(pc, sample);
    sample.demands = 0;
    std::fill(sample.confirmed.begin(), sample.confirmed.end(), 0);
    std::fill(sample.issued.begin(), sample.issued.end(), 0);
  }

  if (dead) {
    sample.deads++;
    if (sample.deads >= alecto_params::DEAD_THRESH) {
      kill_pc(pc);
      _dead_events++;
    }
  } else if (sample.deads > 0) {
    sample.deads--;
  }
}

// ─── Phase control ───────────────────────────────────────────────────────────────

// Fold the phase's measured accuracy into the allocation table's per-sub-prefetcher state machine.
void alecto::end_of_phase(uint64_t pc, const SampleEntry& data)
{
  std::vector<double> accuracy(_n, -1.0);
  for (std::size_t i = 0; i < _n; ++i) {
    if (data.issued[i] > 0) {
      accuracy[i] = static_cast<double>(data.confirmed[i]) / static_cast<double>(data.issued[i]);
    }
  }

  auto& alloc = _allocation_table[pc_index(pc)];

  if (alloc.valid && alloc.tag == data.tag) {
    // Update an existing entry.
    for (std::size_t i = 0; i < _n; ++i) {
      int8_t& s = alloc.states[i];
      if (s > 0) {
        if (accuracy[i] >= alecto_params::ACC_HIGH && s < alecto_params::MAX_STATE) {
          s++;
        } else if (accuracy[i] < alecto_params::ACC_HIGH) {
          s--;
        }
      } else if (s == 0) {
        if (accuracy[i] >= alecto_params::ACC_HIGH) {
          s = 1;
        } else if (accuracy[i] >= 0.0 && accuracy[i] < alecto_params::ACC_LOW) {
          s = alecto_params::BLOCK_STATE;
        }
      } else { // s < 0 : blocked, count back up toward 0
        if (s < -1) {
          s++;
        } else {
          bool any_active = false;
          for (std::size_t j = 0; j < _n; ++j) {
            if (alloc.states[j] > 0) {
              any_active = true;
              break;
            }
          }
          if (!any_active) {
            s = 0; // reactivate once nothing else is carrying the PC
          }
        }
      }
    }
  } else {
    // Allocate a fresh entry from the phase's accuracy profile.
    alloc.valid = true;
    alloc.tag = data.tag;

    bool high_acc = false;
    bool non_temporal = false;
    std::vector<int8_t> init(_n, 0);
    for (std::size_t i = 0; i < _n; ++i) {
      if (accuracy[i] >= alecto_params::ACC_HIGH) {
        init[i] = 1;
        high_acc = true;
        if (i + 1 < _n) {
          non_temporal = true; // a non-"temporal" (non-last) sub-prefetcher is accurate
        }
      } else if (accuracy[i] >= 0.0 && accuracy[i] < alecto_params::ACC_LOW) {
        init[i] = alecto_params::BLOCK_STATE;
      }
    }
    // If any sub-prefetcher is accurate, suppress the merely-mediocre ones.
    if (high_acc) {
      for (std::size_t i = 0; i < _n; ++i) {
        if (init[i] == 0) {
          init[i] = -1;
        }
      }
    }
    // Prefer the accurate non-temporal sub-prefetcher over the (last) temporal one.
    if (high_acc && non_temporal && _n > 0 && init[_n - 1] >= 0) {
      init[_n - 1] = -1;
    }
    alloc.states = init;
  }

  _phase_count++;
}

// A PC has gone dead: stop prefetching for it and clear its accounting. Mirrors Dead().
void alecto::kill_pc(uint64_t pc)
{
  const std::size_t sample = sample_index(pc);
  const std::size_t alloc = pc_index(pc);
  _sample_table[sample].demands = 0;
  for (std::size_t i = 0; i < _n; ++i) {
    _allocation_table[alloc].states[i] = 0;
    _sample_table[sample].confirmed[i] = 0;
    _sample_table[sample].issued[i] = 0;
  }
}

// ─── Candidate handling ──────────────────────────────────────────────────────────

// Apply the degree budget and sandbox dedup to each candidate, issue the survivors, and record the
// issue in the sandbox and sample tables. Mirrors the prefetch-issuing half of IssuePrefetch().
void alecto::process_candidates(std::vector<margo_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {
    auto& stats = _pf_stats.at(prefetcher_id);
    stats.candidates++;

    // Degree gate: only "produce" this candidate while the issuing sub-prefetcher still has budget for
    // the in-flight demand.
    const bool gate = (_issue_budget[prefetcher_id] > 0);
    if (!gate) {
      stats.throttled++;
      if (candidate.notify_result) {
        candidate.notify_result(false, true); // throttled: let the sub-prefetcher's lookahead wind down
      }
      continue;
    }
    _issue_budget[prefetcher_id]--;
    stats.produced++;

    // Count the produced candidate against the issuing PC's sample entry, creating/claiming the entry
    // as needed (this is where a PC becomes "tracked").
    const uint64_t pc = candidate.trigger_ip.to<uint64_t>();
    const std::size_t s_index = sample_index(pc);
    const uint16_t s_tag = pc_tag(pc);
    auto& sample = _sample_table[s_index];
    if (!(sample.valid && sample.tag == s_tag)) {
      if (sample.valid) {
        end_of_phase(sample.pc, sample); // flush the evicted PC's phase before reuse
      }
      sample.valid = true;
      sample.pc = pc;
      sample.tag = s_tag;
      sample.demands = 0;
      sample.deads = 0;
      sample.issued_since_demand = 0;
      std::fill(sample.confirmed.begin(), sample.confirmed.end(), 0);
      std::fill(sample.issued.begin(), sample.issued.end(), 0);
    }
    sample.issued[prefetcher_id]++;
    sample.issued_since_demand++;

    // Sandbox dedup: a block that is already tracked was recently prefetched, so don't re-issue it; we
    // still (re)record the issuer so a later demand hit confirms the right sub-prefetcher.
    const uint64_t block = block_of(candidate.address);
    const std::size_t sb_index = sandbox_index(block);
    const uint8_t sb_tag = sandbox_tag(block);
    auto& sandbox = _sandbox_table[sb_index];

    // Stats-only residency probe (does not gate prefetch_line()).
    const champsim::address pf_line_addr{champsim::block_number{candidate.address}};
    const bool redundant = intern_->in_cache(pf_line_addr) || intern_->in_mshr(pf_line_addr);

    bool issued = false;
    const bool fresh = !(sandbox.valid && sandbox.tag == sb_tag);
    if (fresh) {
      sandbox.valid = true;
      sandbox.tag = sb_tag;
      std::fill(sandbox.issued.begin(), sandbox.issued.end(), false);
      issued = prefetch_line(candidate.address, candidate.fill_this_level, candidate.metadata);
      if (redundant) {
        stats.redundant++;
      } else if (issued) {
        stats.issued++;
      } else {
        stats.not_issued++;
      }
    } else {
      stats.deduped++;
    }
    sandbox.issued[prefetcher_id] = true;
    sandbox.trigger[prefetcher_id] = candidate.trigger_ip;

    // The sub-prefetcher's lookahead may continue whenever the candidate was produced (issued, or
    // dropped only because it was already covered).
    if (candidate.notify_result) {
      candidate.notify_result(issued, !fresh);
    }
  }
}

// ─── ChampSim prefetcher hooks ─────────────────────────────────────────────────────

void alecto::prefetcher_initialize()
{
  for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
    wrapped_prefetcher->initialize();
  }

  std::cout << "ALECTO controller: " << _n << " sub-prefetchers, sample/alloc tables=" << alecto_params::ALLOCATION_TABLE_SIZE
            << ", sandbox=" << alecto_params::SANDBOX_TABLE_SIZE << ", phase=" << alecto_params::PERIOD << " demands" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _n; ++prefetcher_id) {
    std::cout << "    [" << prefetcher_id << "] " << sub_prefetcher_name(prefetcher_id) << std::endl;
  }
}

void alecto::prefetcher_cycle_operate()
{
  for (std::size_t prefetcher_id = 0; prefetcher_id < _n; ++prefetcher_id) {
    auto candidates = _wrapped_prefetchers[prefetcher_id]->cycle_operate();
    process_candidates(candidates, prefetcher_id);
  }
}

uint32_t alecto::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                          uint32_t metadata_in)
{
  const bool is_demand = (type != access_type::PREFETCH);
  if (is_demand) {
    access(ip, addr);     // refresh degree budget + confirm useful prefetches
    account_demand(ip);   // phase / dead bookkeeping for the demanding PC
  }

  // Dynamic demand request allocation: route the request only to the sub-prefetchers Alecto has
  // identified as suitable for this PC. Blocked (IB-state) sub-prefetchers are skipped entirely — they
  // do not see the access, so they neither train on it nor produce candidates from it. This is the DDRA
  // property from the paper (§IV-B/E) that reduces prefetcher-table conflicts, as opposed to only gating
  // the emitted prefetches after the fact.
  compute_allocation(ip);

  uint32_t metadata = metadata_in;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _n; ++prefetcher_id) {
    if (!_demand_allowed[prefetcher_id]) {
      if (is_demand) {
        _pf_stats[prefetcher_id].demand_filtered++;
      }
      continue;
    }
    if (is_demand) {
      _pf_stats[prefetcher_id].demand_trained++;
    }
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, prefetcher_id);
  }

  return metadata;
}

uint32_t alecto::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  uint32_t metadata = metadata_in;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _n; ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_fill(addr, set, way, prefetch, evicted_addr, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, prefetcher_id);
  }

  return metadata;
}

void alecto::prefetcher_final_stats()
{
  const auto pct = [](uint64_t num, uint64_t denom) -> double {
    return denom == 0 ? 0.0 : (static_cast<double>(num) * 100.0) / static_cast<double>(denom);
  };

  const auto sum = [&](uint64_t prefetch_candidate_stats::*field) {
    return std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                           [field](uint64_t acc, const prefetch_candidate_stats& s) { return acc + s.*field; });
  };

  const uint64_t total_candidates = sum(&prefetch_candidate_stats::candidates);
  const uint64_t total_produced = sum(&prefetch_candidate_stats::produced);
  const uint64_t total_issued = sum(&prefetch_candidate_stats::issued);
  const uint64_t total_redundant = sum(&prefetch_candidate_stats::redundant);
  const uint64_t total_throttled = sum(&prefetch_candidate_stats::throttled);
  const uint64_t total_deduped = sum(&prefetch_candidate_stats::deduped);
  const uint64_t total_not_issued = sum(&prefetch_candidate_stats::not_issued);
  const uint64_t total_useful = sum(&prefetch_candidate_stats::useful);
  const uint64_t total_trained = sum(&prefetch_candidate_stats::demand_trained);
  const uint64_t total_filtered = sum(&prefetch_candidate_stats::demand_filtered);
  const uint64_t total_demand_routes = total_trained + total_filtered;

  std::cout << "================================================" << std::endl;
  std::cout << "ALECTO Prefetch Candidate Stats" << std::endl;
  std::cout << "  Learning phases: " << _phase_count << ", dead-PC resets: " << _dead_events << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Demand allocations (training occurrences): trained " << total_trained << ", filtered " << total_filtered << " ("
            << pct(total_filtered, total_demand_routes) << "% of demands withheld from blocked sub-prefetchers)" << std::endl;
  std::cout << "  Total candidates: " << total_candidates << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Issued:    " << total_issued << " (" << pct(total_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Redundant (in cache/MSHR): " << total_redundant << " (" << pct(total_redundant, total_candidates) << "%)" << std::endl;
  std::cout << "  Throttled: " << total_throttled << " (" << pct(total_throttled, total_candidates) << "%)" << std::endl;
  std::cout << "  Deduped:   " << total_deduped << " (" << pct(total_deduped, total_candidates) << "%)" << std::endl;
  std::cout << "  Selected but not issued: " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Confirmed useful: " << total_useful << " (" << pct(total_useful, total_produced) << "% of produced)" << std::endl;

  std::cout << std::endl
            << "  Per sub-prefetcher (trained | filtered | candidates | produced | issued | redundant | useful | throttled | deduped):" << std::endl;
  for (std::size_t prefetcher_id = 0; prefetcher_id < _pf_stats.size(); ++prefetcher_id) {
    const auto& stats = _pf_stats[prefetcher_id];
    const uint64_t routes = stats.demand_trained + stats.demand_filtered;
    std::cout << "    " << sub_prefetcher_name(prefetcher_id) << ": trained=" << stats.demand_trained << " | filtered=" << stats.demand_filtered
              << " (" << pct(stats.demand_filtered, routes) << "%)"
              << " | cand=" << stats.candidates << " | produced=" << stats.produced << " | issued=" << stats.issued << " ("
              << pct(stats.issued, total_issued) << "% of issued)"
              << " | redundant=" << stats.redundant << " | useful=" << stats.useful << " (acc " << pct(stats.useful, stats.produced) << "%)"
              << " | throttled=" << stats.throttled << " | deduped=" << stats.deduped << std::endl;
  }
  std::cout << "================================================" << std::endl;
}
