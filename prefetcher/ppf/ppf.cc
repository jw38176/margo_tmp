#include "ppf.h"

#include <iomanip>
#include <iostream>
#include <numeric>

#include "../sub_prefetcher/sub_prefetcher_config.hh"
#include "cache.h"
#include "champsim.h"

// ─── Hash (Robert Jenkins' mix, same as SPP-PPF) ────────────────────

uint64_t ppf::hash(uint64_t key)
{
  key += (key << 12);
  key ^= (key >> 22);
  key += (key << 4);
  key ^= (key >> 9);
  key += (key << 10);
  key ^= (key >> 2);
  key += (key << 7);
  key ^= (key >> 12);
  key = (key >> 3) * 2654435761;
  return key;
}

// ─── Constructor ────────────────────────────────────────────────────

ppf::ppf(CACHE* cache) : prefetcher(cache)
{
  for (const auto& sp : margo_wrappers::SUB_PREFETCHERS) {
    _wrapped_prefetchers.emplace_back(sp.make(cache));
  }
  _n = _wrapped_prefetchers.size();
  _pf_stats.assign(_n, {});

  // One independent perceptron prefetch filter per sub-prefetcher.
  _filters.resize(_n);

  _perc_depth[0] = ppf_params::PERC_ENTRIES / 2;
  _perc_depth[1] = ppf_params::PERC_ENTRIES;
  _perc_depth[2] = ppf_params::PERC_ENTRIES;
  _perc_depth[3] = ppf_params::PERC_ENTRIES;
  _perc_depth[4] = ppf_params::PERC_ENTRIES / 4;
  _perc_depth[5] = ppf_params::PERC_ENTRIES;
  _perc_depth[6] = ppf_params::PERC_ENTRIES / 64;
  _perc_depth[7] = ppf_params::PERC_ENTRIES / 2;
}

// ═══════════════════════════════════════════════════════════════════
// Perceptron
// ═══════════════════════════════════════════════════════════════════

void ppf::perc_get_index(uint64_t pf_addr, uint64_t trigger_ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3,
                         int64_t delta, uint64_t perc_set[ppf_params::PERC_FEATURES]) const
{
  uint64_t cache_line = pf_addr >> LOG2_BLOCK_SIZE;
  uint64_t page_addr = pf_addr >> LOG2_PAGE_SIZE;
  uint64_t page_offset = cache_line & ((1ull << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE)) - 1);

  uint64_t pre_hash[ppf_params::PERC_FEATURES];
  pre_hash[0] = pf_addr;
  pre_hash[1] = cache_line;
  pre_hash[2] = page_addr;
  pre_hash[3] = trigger_ip ^ pf_addr;
  pre_hash[4] = trigger_ip;
  pre_hash[5] = ip_1 ^ (ip_2 >> 1) ^ (ip_3 >> 2);
  pre_hash[6] = page_offset;
  pre_hash[7] = static_cast<uint64_t>(delta);

  for (int i = 0; i < ppf_params::PERC_FEATURES; i++) {
    perc_set[i] = pre_hash[i] % static_cast<uint64_t>(_perc_depth[i]);
  }
}

int32_t ppf::perc_predict(const PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3,
                          int64_t delta) const
{
  uint64_t perc_set[ppf_params::PERC_FEATURES];
  perc_get_index(pf_addr, trigger_ip, ip_1, ip_2, ip_3, delta, perc_set);

  int32_t sum = 0;
  for (int i = 0; i < ppf_params::PERC_FEATURES; i++) {
    sum += pf.weights[perc_set[i]][i];
  }
  return sum;
}

void ppf::perc_update(PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3,
                      int64_t delta, bool predicted_pf, bool direction, int32_t perc_sum)
{
  uint64_t perc_set[ppf_params::PERC_FEATURES];
  perc_get_index(pf_addr, trigger_ip, ip_1, ip_2, ip_3, delta, perc_set);

  if (!direction) {
    // Prediction was wrong
    for (int i = 0; i < ppf_params::PERC_FEATURES; i++) {
      if (predicted_pf) {
        // Prefetched but useless — train away from prefetching
        if (pf.weights[perc_set[i]][i] > -(ppf_params::PERC_COUNTER_MAX + 1))
          pf.weights[perc_set[i]][i]--;
      } else {
        // Rejected but demanded — train toward prefetching
        if (pf.weights[perc_set[i]][i] < ppf_params::PERC_COUNTER_MAX)
          pf.weights[perc_set[i]][i]++;
      }
    }
  } else if (perc_sum > ppf_params::NEG_UPDT_THRESHOLD && perc_sum < ppf_params::POS_UPDT_THRESHOLD) {
    // Correct but not saturated — reinforce
    for (int i = 0; i < ppf_params::PERC_FEATURES; i++) {
      if (predicted_pf) {
        if (pf.weights[perc_set[i]][i] < ppf_params::PERC_COUNTER_MAX)
          pf.weights[perc_set[i]][i]++;
      } else {
        if (pf.weights[perc_set[i]][i] > -(ppf_params::PERC_COUNTER_MAX + 1))
          pf.weights[perc_set[i]][i]--;
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════
// Filter operations
// ═══════════════════════════════════════════════════════════════════

void ppf::filter_train_demand(PerceptronFilter& pf, uint64_t addr)
{
  uint64_t cache_line = addr >> LOG2_BLOCK_SIZE;
  uint64_t h = hash(cache_line);

  uint64_t quotient = (h >> ppf_params::REMAINDER_BIT) & ((1 << ppf_params::QUOTIENT_BIT) - 1);
  uint64_t remainder = h % (1 << ppf_params::REMAINDER_BIT);

  uint64_t quotient_rej = (h >> ppf_params::REMAINDER_BIT_REJ) & ((1 << ppf_params::QUOTIENT_BIT_REJ) - 1);
  uint64_t remainder_rej = h % (1 << ppf_params::REMAINDER_BIT_REJ);

  auto& entry = pf.filter[quotient];

  // Positive training: demand hits a prefetched line
  if (entry.valid && entry.remainder_tag == remainder && !entry.useful) {
    entry.useful = true;
    pf.pf_useful++;
    perc_update(pf, entry.pf_addr, entry.trigger_ip, entry.ip_1, entry.ip_2, entry.ip_3,
                entry.delta, /*predicted_pf=*/true, /*direction=*/true, entry.perc_sum);
  }

  // Negative training on reject filter: demand hits a line we rejected
  if (!(entry.valid && entry.remainder_tag == remainder)) {
    auto& rej = pf.reject_filter[quotient_rej];
    if (rej.valid && rej.remainder_tag == remainder_rej) {
      perc_update(pf, rej.pf_addr, rej.trigger_ip, rej.ip_1, rej.ip_2, rej.ip_3,
                  rej.delta, /*predicted_pf=*/false, /*direction=*/false, rej.perc_sum);
      rej.valid = false;
      rej.remainder_tag = 0;
    }
  }
}

void ppf::filter_train_evict(PerceptronFilter& pf, uint64_t addr)
{
  uint64_t cache_line = addr >> LOG2_BLOCK_SIZE;
  uint64_t h = hash(cache_line);

  uint64_t quotient = (h >> ppf_params::REMAINDER_BIT) & ((1 << ppf_params::QUOTIENT_BIT) - 1);
  uint64_t remainder = h % (1 << ppf_params::REMAINDER_BIT);
  uint64_t quotient_rej = (h >> ppf_params::REMAINDER_BIT_REJ) & ((1 << ppf_params::QUOTIENT_BIT_REJ) - 1);
  uint64_t remainder_rej = h % (1 << ppf_params::REMAINDER_BIT_REJ);

  auto& entry = pf.filter[quotient];

  // Only the entry whose complete filter fingerprint matches the evicted line
  // may be trained or cleared. Another line can share the same quotient.
  if (entry.valid && entry.remainder_tag == remainder) {
    // Negative training: prefetched but evicted without a demand hit
    if (!entry.useful) {
      if (pf.pf_useful > 0)
        pf.pf_useful--;
      perc_update(pf, entry.pf_addr, entry.trigger_ip, entry.ip_1, entry.ip_2, entry.ip_3,
                  entry.delta, /*predicted_pf=*/true, /*direction=*/false, entry.perc_sum);
    }

    entry.valid = false;
    entry.useful = false;
    entry.remainder_tag = 0;
  }

  auto& rej = pf.reject_filter[quotient_rej];
  if (rej.valid && rej.remainder_tag == remainder_rej) {
    rej.valid = false;
    rej.remainder_tag = 0;
  }
}

bool ppf::filter_check_dup(const PerceptronFilter& pf, uint64_t pf_addr) const
{
  uint64_t cache_line = pf_addr >> LOG2_BLOCK_SIZE;
  uint64_t h = hash(cache_line);
  uint64_t quotient = (h >> ppf_params::REMAINDER_BIT) & ((1 << ppf_params::QUOTIENT_BIT) - 1);
  uint64_t remainder = h % (1 << ppf_params::REMAINDER_BIT);

  const auto& entry = pf.filter[quotient];
  return (entry.valid || entry.useful) && entry.remainder_tag == remainder;
}

void ppf::filter_add(PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, int64_t delta, int32_t perc_sum)
{
  uint64_t cache_line = pf_addr >> LOG2_BLOCK_SIZE;
  uint64_t h = hash(cache_line);
  uint64_t quotient = (h >> ppf_params::REMAINDER_BIT) & ((1 << ppf_params::QUOTIENT_BIT) - 1);
  uint64_t remainder = h % (1 << ppf_params::REMAINDER_BIT);

  auto& entry = pf.filter[quotient];
  entry.valid = true;
  entry.useful = false;
  entry.remainder_tag = remainder;
  entry.pf_addr = pf_addr;
  entry.trigger_ip = trigger_ip;
  entry.ip_1 = _ip_1;
  entry.ip_2 = _ip_2;
  entry.ip_3 = _ip_3;
  entry.delta = delta;
  entry.perc_sum = perc_sum;
}

void ppf::reject_add(PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, int64_t delta, int32_t perc_sum)
{
  uint64_t cache_line = pf_addr >> LOG2_BLOCK_SIZE;
  uint64_t h = hash(cache_line);

  // Skip if already tracked in the main filter
  uint64_t quotient = (h >> ppf_params::REMAINDER_BIT) & ((1 << ppf_params::QUOTIENT_BIT) - 1);
  uint64_t remainder = h % (1 << ppf_params::REMAINDER_BIT);
  const auto& main = pf.filter[quotient];
  if ((main.valid || main.useful) && main.remainder_tag == remainder)
    return;

  uint64_t quotient_rej = (h >> ppf_params::REMAINDER_BIT_REJ) & ((1 << ppf_params::QUOTIENT_BIT_REJ) - 1);
  uint64_t remainder_rej = h % (1 << ppf_params::REMAINDER_BIT_REJ);

  auto& entry = pf.reject_filter[quotient_rej];
  entry.valid = true;
  entry.remainder_tag = remainder_rej;
  entry.pf_addr = pf_addr;
  entry.trigger_ip = trigger_ip;
  entry.ip_1 = _ip_1;
  entry.ip_2 = _ip_2;
  entry.ip_3 = _ip_3;
  entry.delta = delta;
  entry.perc_sum = perc_sum;
}

// ═══════════════════════════════════════════════════════════════════
// Candidate handling
// ═══════════════════════════════════════════════════════════════════

void ppf::process_candidates(std::vector<margo_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id, uint64_t base_addr)
{
  auto& pf = _filters.at(prefetcher_id);
  auto& stats = _pf_stats.at(prefetcher_id);

  for (auto& candidate : candidates) {
    stats.candidates++;

    uint64_t pf_addr = candidate.address.to<uint64_t>();
    uint64_t trigger_ip = candidate.trigger_ip.to<uint64_t>();
    int64_t delta = static_cast<int64_t>(pf_addr >> LOG2_BLOCK_SIZE) - static_cast<int64_t>(base_addr >> LOG2_BLOCK_SIZE);

    int32_t perc_sum = perc_predict(pf, pf_addr, trigger_ip, _ip_1, _ip_2, _ip_3, delta);

    bool do_pf = (perc_sum >= ppf_params::PERC_THRESHOLD_LO);
    bool fill_this_level = (perc_sum >= ppf_params::PERC_THRESHOLD_HI);

    if (!do_pf) {
      reject_add(pf, pf_addr, trigger_ip, delta, perc_sum);
      stats.rejected++;
      _perc_reject++;
      if (candidate.notify_result)
        candidate.notify_result(false, true);
      continue;
    }

    if (filter_check_dup(pf, pf_addr)) {
      stats.filtered_dup++;
      if (candidate.notify_result)
        candidate.notify_result(false, false);
      continue;
    }

    bool issued = prefetch_line(candidate.address, fill_this_level, candidate.metadata);

    if (issued) {
      filter_add(pf, pf_addr, trigger_ip, delta, perc_sum);
      stats.issued++;
      _perc_pass++;
      pf.pf_issued++;
      if (pf.pf_issued > ppf_params::GLOBAL_COUNTER_MAX) {
        pf.pf_issued >>= 1;
        pf.pf_useful >>= 1;
      }
    } else {
      stats.not_issued++;
    }

    if (candidate.notify_result)
      candidate.notify_result(issued, false);
  }
}

// ═══════════════════════════════════════════════════════════════════
// ChampSim prefetcher interface
// ═══════════════════════════════════════════════════════════════════

const char* ppf::sub_prefetcher_name(std::size_t prefetcher_id) { return margo_wrappers::sub_prefetcher_name(prefetcher_id); }

void ppf::prefetcher_initialize()
{
  for (auto& wp : _wrapped_prefetchers)
    wp->initialize();

  std::cout << "PPF (Perceptron Prefetch Filter): " << _n << " sub-prefetchers" << std::endl;
  std::cout << "  PERC_ENTRIES=" << ppf_params::PERC_ENTRIES << " PERC_FEATURES=" << ppf_params::PERC_FEATURES << std::endl;
  std::cout << "  THRESHOLD_HI=" << ppf_params::PERC_THRESHOLD_HI << " THRESHOLD_LO=" << ppf_params::PERC_THRESHOLD_LO << std::endl;
  for (std::size_t i = 0; i < _n; i++)
    std::cout << "    [" << i << "] " << sub_prefetcher_name(i) << std::endl;
}

void ppf::prefetcher_cycle_operate()
{
  for (std::size_t i = 0; i < _n; i++) {
    auto candidates = _wrapped_prefetchers[i]->cycle_operate();
    process_candidates(candidates, i, _last_addr);
  }
}

uint32_t ppf::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                       uint32_t metadata_in)
{
  _ip_3 = _ip_2;
  _ip_2 = _ip_1;
  _ip_1 = _ip_0;
  _ip_0 = ip.to<uint64_t>();
  _last_addr = addr.to<uint64_t>();

  // Train every sub-prefetcher's independent filter with this demand access.
  for (auto& pf : _filters)
    filter_train_demand(pf, addr.to<uint64_t>());

  uint32_t metadata = metadata_in;
  for (std::size_t i = 0; i < _n; i++) {
    auto result = _wrapped_prefetchers[i]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, i, addr.to<uint64_t>());
  }

  return metadata;
}

uint32_t ppf::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  if (evicted_addr != champsim::address{}) {
    // Train every sub-prefetcher's independent filter with this eviction.
    for (auto& pf : _filters)
      filter_train_evict(pf, evicted_addr.to<uint64_t>());
  }

  uint32_t metadata = metadata_in;
  for (std::size_t i = 0; i < _n; i++) {
    auto result = _wrapped_prefetchers[i]->cache_fill(addr, set, way, prefetch, evicted_addr, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, i, addr.to<uint64_t>());
  }

  return metadata;
}

// ═══════════════════════════════════════════════════════════════════
// Final stats
// ═══════════════════════════════════════════════════════════════════

void ppf::prefetcher_final_stats()
{
  const auto pct = [](uint64_t num, uint64_t denom) -> double {
    return denom == 0 ? 0.0 : (static_cast<double>(num) * 100.0) / static_cast<double>(denom);
  };

  const auto sum_field = [&](uint64_t PrefetchStats::*field) {
    return std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                           [field](uint64_t acc, const PrefetchStats& s) { return acc + s.*field; });
  };

  const uint64_t total_candidates = sum_field(&PrefetchStats::candidates);
  const uint64_t total_issued = sum_field(&PrefetchStats::issued);
  const uint64_t total_rejected = sum_field(&PrefetchStats::rejected);
  const uint64_t total_filtered_dup = sum_field(&PrefetchStats::filtered_dup);
  const uint64_t total_not_issued = sum_field(&PrefetchStats::not_issued);

  const uint64_t total_useful = std::accumulate(_filters.begin(), _filters.end(), uint64_t{0},
                                                [](uint64_t acc, const PerceptronFilter& f) { return acc + f.pf_useful; });
  const uint64_t total_pf_issued = std::accumulate(_filters.begin(), _filters.end(), uint64_t{0},
                                                   [](uint64_t acc, const PerceptronFilter& f) { return acc + f.pf_issued; });
  const uint64_t global_accuracy = total_pf_issued ? ((100 * total_useful) / total_pf_issued) : 0;

  std::cout << "================================================" << std::endl;
  std::cout << "PPF (Perceptron Prefetch Filter) Stats" << std::endl;
  std::cout << "  Total candidates: " << total_candidates << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Issued:      " << total_issued << " (" << pct(total_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Rejected:    " << total_rejected << " (" << pct(total_rejected, total_candidates) << "%)" << std::endl;
  std::cout << "  Dup filtered:" << total_filtered_dup << " (" << pct(total_filtered_dup, total_candidates) << "%)" << std::endl;
  std::cout << "  Not issued:  " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Perc pass: " << _perc_pass << ", reject: " << _perc_reject << std::endl;
  std::cout << "  Global accuracy: " << global_accuracy << "% (useful=" << total_useful << ", issued=" << total_pf_issued << ")" << std::endl;

  std::cout << std::endl << "  Per sub-prefetcher (candidates | issued | rejected | dup | not_issued | accuracy):" << std::endl;
  for (std::size_t i = 0; i < _pf_stats.size(); i++) {
    const auto& s = _pf_stats[i];
    const auto& f = _filters[i];
    const uint64_t acc = f.pf_issued ? ((100 * f.pf_useful) / f.pf_issued) : 0;
    std::cout << "    " << sub_prefetcher_name(i) << ": cand=" << s.candidates << " | issued=" << s.issued << " (" << pct(s.issued, total_issued)
              << "% of issued)" << " | rejected=" << s.rejected << " | dup=" << s.filtered_dup << " | not_issued=" << s.not_issued
              << " | acc=" << acc << "% (useful=" << f.pf_useful << ", issued=" << f.pf_issued << ")" << std::endl;
  }
  std::cout << "================================================" << std::endl;
}
