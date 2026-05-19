#ifndef PREFETCHER_PUMPKIN_H
#define PREFETCHER_PUMPKIN_H

#include <cstdint>
#include <memory>
#include <vector>

#include "address.h"
#include "controller/controller.hh"
#include "modules.h"
#include "pumpkin_parameters.hh"
#include "sampler/sampler.hh"
#include "wrapped_prefetcher.hh"

struct pumpkin : public champsim::modules::prefetcher {
  explicit pumpkin(CACHE* cache);

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();

private:
  Sampler _sampler{pumpkin_params::SAMPLER_SETS, pumpkin_params::SAMPLER_WAYS};
  std::unique_ptr<Controller> _controller;
  std::vector<std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher>> _wrapped_prefetchers;

  uint64_t _rng_state{0xD1B54A32D192ED03ULL};
  uint64_t rand_u64();
  void process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, uint64_t current_cycle, std::size_t prefetcher_id);
};

#endif
