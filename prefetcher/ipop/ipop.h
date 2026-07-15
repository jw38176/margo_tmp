#ifndef PREFETCHER_IPOP_H
#define PREFETCHER_IPOP_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../sub_prefetcher/wrapped_prefetcher.hh"
#include "address.h"
#include "ipop_parameters.hh"
#include "modules.h"

// IPOP ("Identify Pollution, Optimise Prefetching") degree controller.
//
// IPOP runs a set of sub-prefetchers (see ../sub_prefetcher/sub_prefetcher_config.hh) and,
// instead of blindly issuing every candidate, it
// continuously measures how much each sub-prefetcher *helps* (useful prefetches) versus how much it
// *hurts* (cache pollution). Every control phase it raises, lowers, or temporarily blocks each
// sub-prefetcher's issue aggressiveness ("degree") based on that net effect and on a global
// congestion signal.
struct ipop : public champsim::modules::prefetcher
{
  explicit ipop(CACHE* cache);

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

  // Forward the simulator-provided LLC reference to any sub-prefetcher that drives LLC way partitioning.
  void set_llc_reference(CACHE* llc)
  {
    for (auto& wrapped_prefetcher : _wrapped_prefetchers) {
      wrapped_prefetcher->set_llc_reference(llc);
    }
  }

private:
  // Lifetime per-sub-prefetcher accounting (printed in final stats).
  struct prefetch_candidate_stats
  {
    uint64_t candidates{0};
    uint64_t issued{0};
    uint64_t throttled{0};  // dropped by degree control (degree gate said "no")
    uint64_t not_issued{0}; // gate said "yes" but prefetch_line() declined (e.g. queue full)
    uint64_t useful{0};     // demand hit on a line this prefetcher brought in
    uint64_t pollution{0};  // demand miss on a line this prefetcher's fill evicted
  };

  // Usefulness-tracking table entry: which sub-prefetcher prefetched this line, and whether the line
  // was served from DRAM (vs an LLC hit) so a later useful hit is charged the right latency.
  struct pf_entry
  {
    uint64_t tag{0};
    bool valid{false};
    bool from_dram{false};
    std::size_t prefetcher_id{0};
  };

  // Pollution-tracking table entry: which sub-prefetcher's fill evicted this line.
  struct poll_entry
  {
    uint64_t tag{0};
    bool valid{false};
    std::size_t prefetcher_id{0};
  };

  std::vector<std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher>> _wrapped_prefetchers;

  // Dynamic per-sub-prefetcher degree.
  //   > 0 : active, value is the current degree (issue aggressiveness)
  //   = 0 : idle, waiting for a calm phase to be reactivated
  //   < 0 : blocked, counts up toward 0 (one step per phase)
  std::vector<int> _current_state;

  // Remaining prefetches each sub-prefetcher may still issue for the in-flight demand request.
  // Replenished to max(current_state, 0) at every demand access and drained as prefetches are issued
  // (including the lookahead continuations that arrive on later cycles before the next demand).
  std::vector<int> _issue_budget;

  // Per-phase effect counters, reset at every control boundary. Useful/pollution events are split by
  // fill source so they can be latency-weighted (LLC-hit latency vs DRAM latency) in run_control().
  std::vector<uint64_t> _useful_l3;
  std::vector<uint64_t> _useful_dram;
  std::vector<uint64_t> _pollution_l3;
  std::vector<uint64_t> _pollution_dram;
  std::vector<uint64_t> _fill_count;

  std::vector<prefetch_candidate_stats> _pf_stats;

  std::vector<pf_entry> _pf_table{ipop_params::PF_ADDR_TABLE_SIZE};
  std::vector<poll_entry> _poll_table{ipop_params::POLL_ADDR_TABLE_SIZE};

  uint64_t _demand_count{0};
  uint64_t _phase_count{0};

  static const char* sub_prefetcher_name(std::size_t prefetcher_id);

  static std::size_t pf_index(uint64_t block);
  static uint64_t pf_tag(uint64_t block);
  static std::size_t poll_index(uint64_t block);
  static uint64_t poll_tag(uint64_t block);

  void replenish_budgets();
  void record_prefetch_issue(champsim::address pf_addr, std::size_t prefetcher_id);
  void note_useful(champsim::address demand_addr);
  void note_pollution_miss(champsim::address demand_addr, bool from_dram);
  void run_control();
  void reset_phase_counters();

  void process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id);
};

#endif
