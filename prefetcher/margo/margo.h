#ifndef PREFETCHER_MARGO_H
#define PREFETCHER_MARGO_H

#include <cstdint>
#include <memory>
#include <vector>

#include "../sub_prefetcher/wrapped_prefetcher.hh"
#include "_rand/rand.hh"
#include "address.h"
#include "controller/controller.hh"
#include "modules.h"
#include "pf_sampler/pf_sampler.hh"
#include "margo_parameters.hh"

struct margo : public champsim::modules::prefetcher {
  explicit margo(CACHE* cache);

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

  // Simulator-provided LLC reference. Forwarded to the controller so training
  // can check whether an evicted line still resides in the LLC.
  void set_llc_reference(CACHE* llc);

private:
  struct prefetch_candidate_stats {
    uint64_t candidates{0};
    uint64_t filtered{0};
    uint64_t issued{0};
    uint64_t redundant{0};  // selected (will_pf) but line already in cache or MSHR
    uint64_t not_issued{0}; // selected (will_pf), not redundant, but prefetch_line declined (PQ full)
  };

  std::vector<prefetch_candidate_stats> _pf_stats;
  std::vector<const char*> _sub_prefetcher_names;
  const char* sub_prefetcher_name(std::size_t prefetcher_id) const;

  PfSampler pf_sampler{margo_params::SAMPLER_SETS, margo_params::SAMPLER_WAYS};
  std::unique_ptr<Controller> controller;
  std::vector<std::unique_ptr<margo_wrappers::WrappedPrefetcher>> _wrapped_prefetchers;

  void process_candidates(std::vector<margo_wrappers::WrappedPrefetch>& candidates, uint64_t current_cycle, std::size_t prefetcher_id);
};

#endif
