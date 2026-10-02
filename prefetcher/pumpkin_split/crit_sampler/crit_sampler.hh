#ifndef PUMPKIN_SPLIT_CRIT_SAMPLER_H
#define PUMPKIN_SPLIT_CRIT_SAMPLER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "../_base_sampler/sampler.hh"
#include "address.h"
#include "msl/lru_table.h"

namespace pumpkin_split_detail
{
// Criticality sampler. Like the prefetch sampler, it address-gates a subset of
// cache lines, but it tracks demand loads (and the prefetches that first request
// a line) to measure whether a load's fill is on the critical path to the
// instruction reaching the head of the ROB. An entry is reported once its
// instruction reaches the ROB head and its line has filled (or was already
// resident when the demand issued).
class CritSampler : public Sampler
{
public:
  explicit CritSampler(std::size_t crit_sampler_sets, std::size_t crit_sampler_ways);
  ~CritSampler();

  void init();

  // Why an entry is reported to the controller. Currently a completed entry is
  // the only terminal event consumed for criticality training.
  enum class EvictionCause
  {
    Complete,
  };

  // Holds the design's [ line_addr | natural_latency | demand_issue_time ] tuple
  // plus the bookkeeping needed to know when both the fill and the ROB-head event
  // have been observed. Criticality is judged counterfactually: had the demand
  // load missed instead of being (pre)fetched, its data could not have arrived
  // before demand_issue_time + natural_latency, and the instruction could not have
  // reached the ROB head before that. Reaching the head earlier means the fetch
  // latency was on the critical path.
  struct crit_sampler_entry
  {
    uint64_t line_tag{0};                // Hashed, truncated tag of the tracked line (storage-efficient)
    champsim::address trigger_pc;        // Demand load PC, used to index the criticality counter
    uint64_t first_request_timestamp{0}; // First real fetch (prefetch or demand miss); kept earliest (masked)
    uint64_t natural_latency{0};         // "Latency": fill_time - first_request_timestamp, frozen at first fill
    uint64_t demand_issue_time{0};       // "Issue Time": cycle the demand load issued from the CPU (masked)
    uint64_t instr_id{0};                // Demand load instruction id
    uint64_t fill_time{0};               // Cycle the line became available in cache (masked, observability)
    uint64_t rob_head_time{0};           // Cycle the demand load first reached the head of the ROB (masked)

    bool has_demand{false};       // A demand load has been associated (instr_id valid)
    bool filled{false};           // Line is resident (fill observed or demand hit) so completion can occur
    bool reached_rob_head{false}; // Demand load has reached the head of the ROB
    bool valid{true};             // Cleared once reported so it is not double-counted

    auto index() const { return line_tag; }
    auto tag() const { return line_tag; }
  };

  struct EvictionEvent
  {
    crit_sampler_entry entry;
    EvictionCause cause;
  };

  // Records a prefetch request for a line. Establishes first_request_timestamp
  // when the line is not already tracked. No instruction is associated.
  void record_prefetch(champsim::address line_addr, uint64_t cycle);

  // Associates the first demand load with a (sampled) line. Later loads to the
  // same line are ignored until that load reaches the ROB head. Records the demand
  // issue time ("Issue Time") and sets instr_id/trigger_pc. On a cache hit it marks
  // the entry resident so a later ROB-head event can complete it, but it does NOT
  // synthesise a fetch latency (a hit performed no fetch); any latency measured
  // from an earlier prefetch fill is preserved. Completion is reported only from
  // notify_rob_head.
  void notify_demand_access(champsim::address line_addr, uint64_t cycle, champsim::address pc, uint64_t instr_id, bool cache_hit);

  // Records the fill time of a line and, on the first fill, freezes its natural
  // (counterfactual) fetch latency = fill_time - first_request_timestamp.
  void notify_cache_fill(champsim::address line_addr, uint64_t cycle);

  // Records the cycle the instruction first reaches the head of the ROB. The core
  // supplies this (not the retirement/commit cycle) because it is the earlier
  // deadline used to judge criticality. Returns a completion event if the line has
  // already filled.
  std::optional<EvictionEvent> notify_rob_head(uint64_t instr_id, uint64_t cycle);

  // Feeds the adaptive gate's cache-churn signal: when a sampled (tracked) line
  // is evicted from the cache, it counts as a cache eviction so the gate can
  // densify if the cache churns faster than this sampler's table. The entry is
  // left intact because criticality is still judged on the eventual ROB-head event.
  void notify_cache_evict(champsim::address line_addr);

  // Number of ROB-head events that matched a tracked demand entry whose line had
  // never been observed as filled (so the entry could not be completed). This
  // breaks the expected invariant that a demand load only reaches the ROB head
  // after its fill, so it is counted as a diagnostic rather than asserted on.
  uint64_t rob_heads_without_fill() const { return _rob_heads_without_fill; }

private:
  std::size_t _crit_sampler_sets;
  std::size_t _crit_sampler_ways;

  champsim::msl::lru_table<crit_sampler_entry> _crit_sampler_table{_crit_sampler_sets, _crit_sampler_ways};

  // Routes ROB-head notifications (which only carry an instr_id) to the
  // line-indexed sampler table via the line's hashed tag.
  std::unordered_map<uint64_t, uint64_t> _instr_id_to_line;

  // Diagnostic: ROB-head events that hit a tracked demand entry that was never filled.
  uint64_t _rob_heads_without_fill{0};

  void forget_instr_id(uint64_t instr_id);
  std::optional<EvictionEvent> try_complete(crit_sampler_entry& entry);

  // Handles an entry displaced by an lru_table::fill. A displaced live entry that
  // was still awaiting completion (has a demand load and wasn't reported) counts
  // as sampler-table pressure for the adaptive gate; its instr_id routing is also
  // dropped.
  void handle_table_eviction(const std::optional<crit_sampler_entry>& evicted);
};

} // namespace pumpkin_split_detail

#endif
