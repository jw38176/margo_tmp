#include "crit_sampler.hh"

#include <cassert>

#include "../pumpkin_masking.hh"

CritSampler::CritSampler(std::size_t crit_sampler_sets, std::size_t crit_sampler_ways)
    : _crit_sampler_sets(crit_sampler_sets), _crit_sampler_ways(crit_sampler_ways)
{
}

CritSampler::~CritSampler() = default;

void CritSampler::init()
{
  Sampler::init();
  _instr_id_to_line.clear();
  _rob_heads_without_fill = 0;
}

void CritSampler::forget_instr_id(uint64_t instr_id)
{
  if (instr_id != 0)
    _instr_id_to_line.erase(instr_id);
}

void CritSampler::handle_table_eviction(const std::optional<crit_sampler_entry>& evicted)
{
  if (!evicted.has_value())
    return;

  // Unfinished entry was evicted before reaching the ROB head
  if (evicted->valid && !evicted->reached_rob_head)
    report_sampler_table_evict();

  if (evicted->has_demand)
    forget_instr_id(evicted->instr_id);
}

// Reports a completed entry: clears it from the table and drops its instr_id
// routing so later evicts/ROB-head events don't double-count.
std::optional<CritSampler::EvictionEvent> CritSampler::try_complete(crit_sampler_entry& entry)
{
  if (!entry.valid || !entry.has_demand || !entry.filled || !entry.reached_rob_head)
    return std::nullopt;

  entry.valid = false;

  crit_sampler_entry probe{};
  probe.line_tag = entry.line_tag;
  _crit_sampler_table.invalidate(probe);
  forget_instr_id(entry.instr_id);

  return EvictionEvent{entry, EvictionCause::Complete};
}

void CritSampler::record_prefetch(champsim::address line_addr, uint64_t cycle)
{
  crit_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(line_addr);

  // Already tracked: keep the existing (earlier) first-request timestamp.
  if (_crit_sampler_table.check_hit(probe).has_value())
    return;

  crit_sampler_entry e{};
  e.line_tag = pumpkin_masking::make_line_tag(line_addr);
  e.first_request_timestamp = cycle;
  e.valid = true;

  handle_table_eviction(_crit_sampler_table.fill(e));
}

void CritSampler::notify_demand_access(champsim::address line_addr, uint64_t cycle, champsim::address pc, uint64_t instr_id, bool cache_hit)
{
  crit_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(line_addr);

  crit_sampler_entry entry{};
  if (auto hit = _crit_sampler_table.check_hit(probe); hit.has_value())
  {
    entry = hit.value();
    if (entry.has_demand)
      return;
  }
  else
  {
    entry.line_tag = pumpkin_masking::make_line_tag(line_addr);
    entry.first_request_timestamp = cycle;
    entry.valid = true;
  }

  entry.has_demand = true;
  entry.instr_id = instr_id;
  entry.trigger_pc = pc;
  entry.demand_issue_time = cycle; // "Issue Time": when the demand issued from the CPU
  if (entry.first_request_timestamp == 0)
    entry.first_request_timestamp = cycle;

  // A hit means the line is already resident, so no fill event will arrive. Mark
  // it resident so a later ROB-head event can complete the entry, but do NOT
  // synthesise a latency: a hit performed no fetch. If an earlier prefetch fill already froze
  // natural_latency, that (correct) value is left untouched.
  if (cache_hit && !entry.filled)
  {
    entry.filled = true;
    entry.fill_time = cycle;
  }

  _instr_id_to_line[instr_id] = pumpkin_masking::make_line_tag(line_addr);

  handle_table_eviction(_crit_sampler_table.fill(entry));
}

void CritSampler::notify_cache_fill(champsim::address line_addr, uint64_t cycle)
{
  crit_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(line_addr);

  auto hit = _crit_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return;

  crit_sampler_entry entry = hit.value();
  if (!entry.filled)
  {
    entry.filled = true;
    entry.fill_time = cycle;
    // Freeze the counterfactual "Latency": how long this line's first real fetch
    // took. first_request_timestamp is kept from the earliest request, so an
    // MSHR-merge fill cannot shrink it below the original (deeper) fetch's cost.
    // Computed with wrap-safe modular subtraction; an implausibly large delta
    // (a wrap artifact) is discarded, leaving natural_latency at 0.
    if (entry.first_request_timestamp != 0)
    {
      const uint64_t d = pumpkin_masking::timestamp_delta(cycle, entry.first_request_timestamp);
      if (d != 0 && d <= pumpkin_params::TIMESTAMP_HALF)
        entry.natural_latency = d;
    }
  }

  _crit_sampler_table.fill(entry);
}

std::optional<CritSampler::EvictionEvent> CritSampler::notify_rob_head(uint64_t instr_id, uint64_t cycle)
{
  // This instruction is not tracked
  auto route = _instr_id_to_line.find(instr_id);
  if (route == _instr_id_to_line.end())
    return std::nullopt;

  crit_sampler_entry probe{};
  probe.line_tag = route->second;

  auto hit = _crit_sampler_table.check_hit(probe);
  if (!hit.has_value())
  {
    _instr_id_to_line.erase(route);
    return std::nullopt;
  }

  crit_sampler_entry entry = hit.value();

  if (!entry.has_demand || entry.instr_id != instr_id)
  {
    _instr_id_to_line.erase(route);
    return std::nullopt;
  }

  if (!entry.reached_rob_head)
  {
    entry.reached_rob_head = true;
    entry.rob_head_time = cycle;
  }

  if (auto evt = try_complete(entry); evt.has_value())
    return evt;

  // The instruction reached the ROB head but this sampler never observed a fill
  // for its line, so the entry cannot be completed. This violates the expected
  // invariant that a demand load only reaches the ROB head after its fill is
  // observed; count it and drop the stale entry so it does not linger or get
  // double-counted.
  ++_rob_heads_without_fill;
  crit_sampler_entry stale{};
  stale.line_tag = entry.line_tag;
  _crit_sampler_table.invalidate(stale);
  forget_instr_id(instr_id);
  return std::nullopt;
}

void CritSampler::notify_cache_evict(champsim::address line_addr)
{
  crit_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(line_addr);

  // Count cache evictions of tracked lines, but keep the entry for pending ROB-head events.
  if (_crit_sampler_table.check_hit(probe).has_value())
    report_cache_evict();
}
