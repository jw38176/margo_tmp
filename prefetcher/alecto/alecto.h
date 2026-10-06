#ifndef PREFETCHER_ALECTO_H
#define PREFETCHER_ALECTO_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../sub_prefetcher/wrapped_prefetcher.hh"
#include "address.h"
#include "alecto_parameters.hh"
#include "modules.h"

// Alecto: a per-PC, accuracy-driven controller for a set of sub-prefetchers.
//
// Alecto runs the configured sub-prefetcher set (see ../sub_prefetcher/sub_prefetcher_config.hh) and learns,
// per program counter, *which* of those sub-prefetchers to trust and *how aggressively* (degree).
// Three tables cooperate:
//
//   * Sandbox table  - recently issued prefetch blocks. Filters duplicate issues and, when a later
//                       demand hits a block it holds, confirms the issuing sub-prefetcher(s) as useful.
//   * Sample table   - per-PC, per-phase accounting of prefetches issued vs. confirmed (accuracy).
//   * Allocation table - per-PC control "state" per sub-prefetcher; the learned accuracy is folded in
//                       at every phase boundary, and the state sets the per-demand issue degree.
//
// Adaptation notes vs. the original (legacy-ChampSim) Alecto:
//   * The legacy version invoked every sub-prefetcher synchronously per demand with set_degree().
//     Pumpkin's wrapped sub-prefetchers emit candidates asynchronously (cache_operate + lookahead via
//     cycle_operate) and do not expose set_degree(), so the degree is enforced as a per-demand issue
//     budget (identical mechanism to IPOP).
//   * "dead" demand detection (a PC whose prefetchers produce nothing) is determined from whether any
//     prefetch was attributed to the PC since its previous demand, because candidates now arrive after
//     the demand returns rather than within it.
struct alecto : public champsim::modules::prefetcher {
  explicit alecto(CACHE* cache);

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
  // ─── Tables ────────────────────────────────────────────────────────────────────

  // Per-sub-prefetcher control state for a PC.
  //   > 0 : active, degree = BASE_DEGREE + state
  //   = 0 : active at BASE_DEGREE
  //   < 0 : blocked, counts up toward 0 (one step per phase)
  struct AllocationEntry {
    bool valid{false};
    uint16_t tag{0};
    std::vector<int8_t> states; // size = number of sub-prefetchers
  };

  // Per-PC accuracy accounting for the current learning phase.
  struct SampleEntry {
    bool valid{false};
    uint64_t pc{0};
    uint16_t tag{0};
    uint16_t demands{0};
    uint16_t deads{0};
    std::vector<uint32_t> issued;    // prefetches produced this phase, per sub-prefetcher
    std::vector<uint32_t> confirmed; // of those, later demanded (useful), per sub-prefetcher
    uint64_t issued_since_demand{0}; // liveness signal for "dead" detection (adaptation)
  };

  // A recently issued prefetch block and who issued it.
  struct SandboxEntry {
    bool valid{false};
    uint8_t tag{0};
    std::vector<bool> issued;                // which sub-prefetchers issued this block
    std::vector<champsim::address> trigger;  // triggering PC per sub-prefetcher
  };

  // ─── Lifetime per-sub-prefetcher accounting (printed in final stats) ─────────────
  struct prefetch_candidate_stats {
    uint64_t candidates{0};      // candidates seen from the sub-prefetcher
    uint64_t produced{0};        // passed the degree gate (these enter the accuracy sample)
    uint64_t issued{0};          // prefetch_line() accepted and line not already in cache/MSHR
    uint64_t redundant{0};       // stats only: selected but line already in cache or MSHR
    uint64_t throttled{0};       // dropped by the degree budget
    uint64_t deduped{0};         // already covered by the sandbox (recently prefetched)
    uint64_t not_issued{0};      // passed gate + dedup, not redundant, but prefetch_line() declined
    uint64_t useful{0};          // later confirmed by a demand hit (counted against produced)
    uint64_t demand_trained{0};  // demands routed to this sub-prefetcher (a training occurrence)
    uint64_t demand_filtered{0}; // demands withheld because this sub-prefetcher was blocked (IB state)
  };

  std::size_t _n{0}; // number of sub-prefetchers
  std::vector<std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher>> _wrapped_prefetchers;
  std::vector<const char*> _sub_prefetcher_names;

  std::vector<AllocationEntry> _allocation_table{alecto_params::ALLOCATION_TABLE_SIZE};
  std::vector<SampleEntry> _sample_table{alecto_params::SAMPLE_TABLE_SIZE};
  std::vector<SandboxEntry> _sandbox_table{alecto_params::SANDBOX_TABLE_SIZE};

  // Per-demand issue budget, replenished from the demanding PC's allocation entry and drained as
  // sub-prefetchers (including their lookahead continuations) emit candidates.
  std::vector<int> _issue_budget;

  // Per-demand routing mask (dynamic demand request allocation): true if the sub-prefetcher is
  // identified as suitable (UI/IA state) for the current PC and may receive the request; false if it is
  // blocked (IB state) and the request must be withheld so it is neither trained nor allowed to issue.
  std::vector<char> _demand_allowed;

  std::vector<prefetch_candidate_stats> _pf_stats;
  uint64_t _phase_count{0};
  uint64_t _dead_events{0};

  const char* sub_prefetcher_name(std::size_t prefetcher_id) const;
  static uint64_t xor_fold(uint64_t key, int index_len);

  static std::size_t pc_index(uint64_t pc);
  static std::size_t sample_index(uint64_t pc);
  static uint16_t pc_tag(uint64_t pc);
  static std::size_t sandbox_index(uint64_t block);
  static uint8_t sandbox_tag(uint64_t block);

  // Demand-path helpers.
  void access(champsim::address ip, champsim::address addr); // degree budget + usefulness credit
  void compute_allocation(champsim::address ip);             // decide which sub-prefetchers get this demand
  void check_sandbox(champsim::address addr);                // confirm useful prefetches on a demand hit
  void confirm(champsim::address ip, std::size_t prefetcher_id);
  void account_demand(champsim::address ip); // demands++, phase rollover, dead handling

  // Phase / state-machine helpers.
  void end_of_phase(uint64_t pc, const SampleEntry& data);
  void kill_pc(uint64_t pc);

  // Candidate handling.
  void process_candidates(std::vector<pumpkin_wrappers::WrappedPrefetch>& candidates, std::size_t prefetcher_id);
};

#endif
