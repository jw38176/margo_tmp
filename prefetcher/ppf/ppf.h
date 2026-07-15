#ifndef PREFETCHER_PPF_H
#define PREFETCHER_PPF_H

/*
 * PPF – Perceptron Prefetch Filter
 *
 * A meta-prefetcher that wraps the configured set of sub-prefetchers (see
 * ../sub_prefetcher/sub_prefetcher_config.hh) and filters their candidates
 * through a perceptron-based prefetch filter before issuing.
 *
 * The filtering mechanism is extracted from the SPP-PPF prefetcher (see
 * ../spp_ppf/) and generalised so that the perceptron features are derived
 * from information available in any WrappedPrefetch candidate (prefetch
 * address, triggering IP, IP history, and sub-prefetcher identity) rather
 * than from SPP-specific signatures and deltas.
 *
 * The multi-prefetcher wrapping pattern follows pumpkin / alecto.
 */

#include <cstdint>
#include <memory>
#include <vector>

#include "../sub_prefetcher/wrapped_prefetcher.hh"
#include "access_type.h"
#include "address.h"
#include "modules.h"
#include "ppf_parameters.hh"

struct ppf : public champsim::modules::prefetcher {
  explicit ppf(CACHE* cache);

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

private:
  // ─── Sub-prefetchers ──────────────────────────────────────────────
  std::size_t _n{0};
  std::vector<std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher>> _wrapped_prefetchers;

  // ─── Perceptron feature layout ────────────────────────────────────
  //
  // Features (replacing SPP-specific signature/delta/confidence/depth):
  //   0  pf_addr                                   depth PERC_ENTRIES/2
  //   1  pf_addr >> LOG2_BLOCK_SIZE  (cache line)   depth PERC_ENTRIES
  //   2  pf_addr >> LOG2_PAGE_SIZE   (page)         depth PERC_ENTRIES
  //   3  trigger_ip ^ page_addr                     depth PERC_ENTRIES
  //   4  trigger_ip ^ prefetcher_id                 depth PERC_ENTRIES/4
  //   5  ip_1 ^ (ip_2>>1) ^ (ip_3>>2)              depth PERC_ENTRIES
  //   6  trigger_ip ^ cache_line                    depth PERC_ENTRIES/4
  //   7  page_addr  ^ prefetcher_id                 depth PERC_ENTRIES/2
  //   8  prefetcher_id                              depth PERC_ENTRIES/32
  //
  // The perceptron feature-index depths are configuration shared by every
  // sub-prefetcher; the weights themselves live in each per-prefetcher filter.
  int32_t _perc_depth[ppf_params::PERC_FEATURES]{};

  // ─── Prefetch filter entry ───────────────────────────────────────
  // Tracks issued prefetches; a later demand hit triggers positive training,
  // an eviction without use triggers negative training.
  struct FilterEntry {
    uint64_t remainder_tag{0};
    uint64_t pf_addr{0};
    uint64_t trigger_ip{0};
    uint64_t ip_1{0}, ip_2{0}, ip_3{0};
    uint32_t prefetcher_id{0};
    int32_t perc_sum{0};
    bool valid{false};
    bool useful{false};
  };

  // ─── Reject filter entry ─────────────────────────────────────────
  // Tracks candidates the perceptron rejected; if a later demand hits
  // one of these, the perceptron is trained negatively (it should have
  // allowed the prefetch).
  struct RejectEntry {
    uint64_t remainder_tag{0};
    uint64_t pf_addr{0};
    uint64_t trigger_ip{0};
    uint64_t ip_1{0}, ip_2{0}, ip_3{0};
    uint32_t prefetcher_id{0};
    int32_t perc_sum{0};
    bool valid{false};
  };

  // ─── Per-prefetcher perceptron prefetch filter ───────────────────
  // Each sub-prefetcher owns an independent perceptron, prefetch filter,
  // reject filter, and accuracy counters, so that one sub-prefetcher's
  // behaviour never trains or filters another's candidates.
  struct PerceptronFilter {
    int32_t weights[ppf_params::PERC_ENTRIES][ppf_params::PERC_FEATURES]{};
    FilterEntry filter[ppf_params::FILTER_SET]{};
    RejectEntry reject_filter[ppf_params::FILTER_SET_REJ]{};
    uint64_t pf_useful{0};
    uint64_t pf_issued{0};
  };
  std::vector<PerceptronFilter> _filters;

  void perc_get_index(uint64_t pf_addr, uint64_t trigger_ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3,
                      uint32_t prefetcher_id, uint64_t perc_set[ppf_params::PERC_FEATURES]) const;
  int32_t perc_predict(const PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3,
                       uint32_t prefetcher_id) const;
  void perc_update(PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3,
                   uint32_t prefetcher_id, bool direction, int32_t perc_sum);

  void filter_train_demand(PerceptronFilter& pf, uint64_t addr);
  void filter_train_evict(PerceptronFilter& pf, uint64_t addr);
  bool filter_check_dup(const PerceptronFilter& pf, uint64_t pf_addr) const;
  void filter_add(PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, uint32_t prefetcher_id, int32_t perc_sum);
  void reject_add(PerceptronFilter& pf, uint64_t pf_addr, uint64_t trigger_ip, uint32_t prefetcher_id, int32_t perc_sum);

  // ─── IP history ──────────────────────────────────────────────────
  uint64_t _ip_0{0}, _ip_1{0}, _ip_2{0}, _ip_3{0};

  // ─── Per-sub-prefetcher stats ────────────────────────────────────
  struct PrefetchStats {
    uint64_t candidates{0};
    uint64_t issued{0};
    uint64_t rejected{0};
    uint64_t filtered_dup{0};
    uint64_t not_issued{0};
  };
  std::vector<PrefetchStats> _pf_stats;
  uint64_t _perc_pass{0};
  uint64_t _perc_reject{0};

  void process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id);

  static uint64_t hash(uint64_t key);
  static const char* sub_prefetcher_name(std::size_t prefetcher_id);
};

#endif
