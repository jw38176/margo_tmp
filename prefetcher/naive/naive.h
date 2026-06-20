#ifndef PREFETCHER_NAIVE_H
#define PREFETCHER_NAIVE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "address.h"
#include "modules.h"
#include "../pumpkin/wrapped_prefetcher.hh"

struct naive : public champsim::modules::prefetcher {
  explicit naive(CACHE* cache);

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

private:
  struct prefetch_candidate_stats {
    uint64_t candidates{0};
    uint64_t issued{0};
    uint64_t not_issued{0};
  };

  std::vector<prefetch_candidate_stats> _pf_stats;
  static const char* sub_prefetcher_name(std::size_t prefetcher_id);

  std::vector<std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher>> _wrapped_prefetchers;

  void process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id);
};

#endif
