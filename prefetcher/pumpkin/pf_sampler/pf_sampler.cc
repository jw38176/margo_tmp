#include "pf_sampler.hh"

#include "../pumpkin_masking.hh"

PfSampler::PfSampler(std::size_t pf_sampler_sets, std::size_t pf_sampler_ways) : _pf_sampler_sets(pf_sampler_sets), _pf_sampler_ways(pf_sampler_ways) {}

PfSampler::~PfSampler() = default;

void PfSampler::init(std::size_t num_prefetchers)
{
  Sampler::init();
  _num_prefetchers = num_prefetchers;
  _cache_hit_events = 0;
  _late_prefetch_events = 0;
}

std::optional<PfSampler::EvictionEvent> PfSampler::record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc,
                                                               std::size_t prefetcher_id, bool prefetched, bool llc_resident)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(pf_line_addr);

  // If a pf_sampler entry already exists for this line, merge into it rather than overwriting.
  if (auto hit = _pf_sampler_table.check_hit(probe); hit.has_value())
  {
    pf_sampler_entry updated = hit.value();

    if (updated.demand_timestamp != 0)
    {
      // Prefetch after demand access - do not record this prefetch
      // This is a late prefetch, waiting for fill latency before positive training.
      return std::nullopt;
    }

    if (updated.fill_latency != 0)
    {
      // Prefetch after cache fill - do not record this prefetch
      // This is a redundant prefetch.
      return std::nullopt;
    }

    if (!updated.useful)
    {
      // Record new prefetch from a different prefetcher for this line
      if (updated.prefetch_timestamps[prefetcher_id] == 0)
      {
        updated.prefetch_timestamps[prefetcher_id] = cycle;
        updated.prefetch_src_pcs[prefetcher_id] = src_pc;
      }

      // Replace a previous prefetch from the same prefetcher if not prefetched
      // TODO

      if (prefetched)
      {
        updated.prefetched = true;
      }
      _pf_sampler_table.fill(updated);
      return std::nullopt;
    }

    if (delay_subtimely_training)
    {
      if (updated.valid)
      {
        // A late prefetch was already consumed by demand and is waiting for fill
        // latency before positive training. Preserve that pending terminal event.
        return std::nullopt;
      }

      // useful==true and valid==false means we've already trained for this entry.
    }
    else
    {
      // useful==true means valid==false and we've already trained for this entry.
    }
    // Overwrite it with a fresh entry below.
  }

  pf_sampler_entry e{};
  e.line_tag = pumpkin_masking::make_line_tag(pf_line_addr);
  e.fill_latency = 0;
  e.first_request_timestamp = cycle;
  if (delay_subtimely_training)
  {
    e.demand_timestamp = 0;
  }
  e.prefetch_timestamps.assign(_num_prefetchers, 0);
  e.prefetch_src_pcs.assign(_num_prefetchers, champsim::address{});
  e.prefetched = prefetched;
  e.in_cache = false;
  e.useful = false;
  e.valid = true;
  e.llc_resident_at_record = llc_resident;

  if (prefetcher_id < _num_prefetchers)
  {
    e.prefetch_timestamps[prefetcher_id] = cycle;
    e.prefetch_src_pcs[prefetcher_id] = src_pc;
  }

  auto evicted = _pf_sampler_table.fill(e);
  if (!evicted.has_value())
  {
    return std::nullopt;
  }

  pf_sampler_entry& victim = evicted.value();

  // Only calibrate on terminal events for prefetched entries that were filled into cache.
  if (victim.prefetched && victim.in_cache && victim.valid && !victim.useful)
  {
    report_sampler_table_evict();
  }

  if (!victim.valid || !victim.prefetched || (delay_subtimely_training && victim.useful))
  {
    return std::nullopt;
  }

  victim.valid = false;
  return EvictionEvent{victim, EvictionCause::PfSamplerCapacity};
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle, bool from_dram)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(pf_line_addr);

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  pf_sampler_entry updated = hit.value();

  // Calculate fill latency using wrap-safe modular subtraction of the masked
  // timestamps. Discard an implausibly large delta (a wrap artifact), mirroring
  // berti's cant_track_latency behaviour.
  if (updated.fill_latency == 0)
  {
    const uint64_t d = pumpkin_masking::timestamp_delta(cycle, updated.first_request_timestamp);
    if (d != 0 && d <= pumpkin_params::TIMESTAMP_HALF)
      updated.fill_latency = d;
  }

  updated.in_cache = true;
  // Record the ground-truth fill origin before any terminal-event snapshot.
  updated.from_dram = from_dram;

  if (delay_subtimely_training)
  {
    // Delayed CacheHit event for late prefetches
    if (updated.useful && updated.valid)
    {
      updated.valid = false;
      pf_sampler_entry snapshot = updated;
      _pf_sampler_table.invalidate(probe);
      _late_prefetch_events++;
      return EvictionEvent{snapshot, EvictionCause::LatePrefetch};
    }
  }

  _pf_sampler_table.fill(updated);
  return std::nullopt;
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_demand_access(champsim::address line_addr, uint64_t cycle, champsim::address demand_pc)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(line_addr);

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  pf_sampler_entry entry = hit.value();
  if (!entry.valid)
  {
    return std::nullopt;
  }

  // Mark entry as used by demand to prevent double-counting.
  entry.useful = true;
  entry.demand_pc = demand_pc;

  if (delay_subtimely_training)
  {
    entry.demand_timestamp = cycle;

    if (!entry.in_cache)
    {
      // This is a late prefetch: keep the entry until cache_fill computes latency.
      _pf_sampler_table.fill(entry);
      return std::nullopt;
    }

    entry.valid = false;
  }
  else
  {
    entry.valid = false;
    entry.in_cache = true;
  }

  // Snapshot the consumed entry for the controller, then invalidate the
  // pf_sampler slot so the LRU can reclaim it without producing an extra
  // PfSamplerCapacity event.
  pf_sampler_entry snapshot = entry;
  _pf_sampler_table.invalidate(probe);

  _cache_hit_events++;
  return EvictionEvent{snapshot, EvictionCause::CacheHit};
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_cache_evict(champsim::address line_addr)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_masking::make_line_tag(line_addr);

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  pf_sampler_entry entry = hit.value();

  // Only valid, filled prefetches generate cache-eviction events.
  if (!entry.prefetched || !entry.in_cache || !entry.valid || entry.useful)
  {
    return std::nullopt;
  }

  // Bookkeeping 
  entry.in_cache = false;
  entry.valid = false;

  report_cache_evict();

  _pf_sampler_table.invalidate(probe);

  return EvictionEvent{entry, EvictionCause::CacheEvict};
}
