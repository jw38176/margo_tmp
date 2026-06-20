#include "naive.h"

#include <iomanip>
#include <iostream>
#include <numeric>

#include "cache.h"
#include "../pumpkin/sub_prefetcher_config.hh"


// USES PUMPKIN'S SET OF SUB-PREFETCHERS
naive::naive(CACHE* cache) : prefetcher(cache)
{
  for (const auto& sub_prefetcher : pumpkin_wrappers::SUB_PREFETCHERS) {
    _wrapped_prefetchers.emplace_back(sub_prefetcher.make(cache));
  }
  _pf_stats.assign(_wrapped_prefetchers.size(), {});
}

const char* naive::sub_prefetcher_name(std::size_t prefetcher_id)
{
  return pumpkin_wrappers::sub_prefetcher_name(prefetcher_id);
}

void naive::process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id)
{
  for (auto& candidate : candidates) {
    const bool issued = prefetch_line(candidate.address, candidate.fill_this_level, candidate.metadata);

    if (candidate.notify_result) {
      candidate.notify_result(issued, false);
    }

    auto& stats = _pf_stats.at(prefetcher_id);
    stats.candidates++;
    if (issued) {
      stats.issued++;
    } else {
      stats.not_issued++;
    }
  }
}

void naive::prefetcher_initialize()
{
  for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
    wrapped_prefetcher->initialize();
  }
}

void naive::prefetcher_cycle_operate()
{
  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto candidates = _wrapped_prefetchers[prefetcher_id]->cycle_operate();
    process_candidates(candidates, prefetcher_id);
  }
}

uint32_t naive::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                         uint32_t metadata_in)
{
  uint32_t metadata = metadata_in;

  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, prefetcher_id);
  }

  return metadata;
}

uint32_t naive::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                      uint32_t metadata_in)
{
  uint32_t metadata = metadata_in;

  for (std::size_t prefetcher_id = 0; prefetcher_id < _wrapped_prefetchers.size(); ++prefetcher_id) {
    auto result = _wrapped_prefetchers[prefetcher_id]->cache_fill(addr, set, way, prefetch, evicted_addr, metadata);
    metadata = result.metadata;
    process_candidates(result.prefetches, prefetcher_id);
  }

  return metadata;
}

void naive::prefetcher_final_stats()
{
  const auto pct = [](uint64_t num, uint64_t denom) -> double {
    return denom == 0 ? 0.0 : (static_cast<double>(num) * 100.0) / static_cast<double>(denom);
  };

  const uint64_t total_candidates =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.candidates; });
  const uint64_t total_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.issued; });
  const uint64_t total_not_issued =
      std::accumulate(_pf_stats.begin(), _pf_stats.end(), uint64_t{0},
                      [](uint64_t sum, const prefetch_candidate_stats& s) { return sum + s.not_issued; });

  std::cout << "================================================" << std::endl;
  std::cout << "NAIVE Prefetch Candidate Stats (no filtering)" << std::endl;
  std::cout << "  Total candidates: " << total_candidates << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Issued:   " << total_issued << " (" << pct(total_issued, total_candidates) << "%)" << std::endl;
  std::cout << "  Not issued: " << total_not_issued << " (" << pct(total_not_issued, total_candidates) << "%)" << std::endl;

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
  std::cout << "================================================" << std::endl;
}
