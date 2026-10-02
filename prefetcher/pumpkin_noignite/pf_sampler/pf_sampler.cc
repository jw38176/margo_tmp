#include "pf_sampler.hh"

#include "../pumpkin_noignite_masking.hh"

namespace pumpkin_noignite_detail
{


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
  probe.line_tag = pumpkin_noignite_masking::make_line_tag(pf_line_addr);

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

    if (updated.training == TrainingState::Pending)
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

    if (updated.training == TrainingState::AwaitingFill)
    {
      // A late prefetch was already consumed by demand and is waiting for fill
      // latency before positive training. Preserve that pending terminal event.
      return std::nullopt;
    }

    // A Trained shadow is being re-prefetched, so the fresh entry below takes over
    // its slot. That ends the shadow's calibration race sampler-first. No terminal
    // event: this entry was already positively trained.
    if (updated.in_calibration_race())
    {
      report_sampler_table_evict();
    }
    // Overwrite it with a fresh entry below.
  }

  pf_sampler_entry e{};
  e.line_tag = pumpkin_noignite_masking::make_line_tag(pf_line_addr);
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
  e.training = TrainingState::Pending;
  e.calibration_live = true;
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

  // The sampler dropped this entry while its line was still in cache, so the race
  // resolves sampler-first. Counted once for unused live prefetches and for Trained
  // shadows alike, so both outcomes of a useful prefetch stay visible to the gate.
  if (victim.in_calibration_race())
  {
    report_sampler_table_evict();
  }

  // Only an unused, issued prefetch still owes a terminal event; a shadow was
  // already trained positively and must not now be trained negatively.
  const bool trainable = victim.calibration_live && victim.prefetched && victim.training == TrainingState::Pending;
  victim.calibration_live = false;

  if (!trainable)
  {
    return std::nullopt;
  }

  return EvictionEvent{victim, EvictionCause::PfSamplerCapacity};
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle, bool from_dram)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_noignite_masking::make_line_tag(pf_line_addr);

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  pf_sampler_entry updated = hit.value();

  // Calculate fill latency using wrap-safe modular subtraction of the masked
  // timestamps. Discard an implausibly large delta (a wrap artifact), mirroring
  // berti's cant_track_latency behaviour.
  if (updated.fill_latency == 0)
  {
    const uint64_t d = pumpkin_noignite_masking::timestamp_delta(cycle, updated.first_request_timestamp);
    if (d != 0 && d <= pumpkin_noignite_params::TIMESTAMP_HALF)
      updated.fill_latency = d;
  }

  updated.in_cache = true;
  // Record the ground-truth fill origin before any terminal-event snapshot.
  updated.from_dram = from_dram;

  if (delay_subtimely_training)
  {
    // Delayed LatePrefetch event: a demand already consumed this line and the fill
    // has now supplied the latency the event needs. Retain the entry as a Trained
    // shadow rather than invalidating it, so it keeps contesting the calibration
    // race; it can no longer emit another event.
    if (updated.training == TrainingState::AwaitingFill)
    {
      updated.training = TrainingState::Trained;
      _pf_sampler_table.fill(updated);
      _late_prefetch_events++;
      return EvictionEvent{updated, EvictionCause::LatePrefetch};
    }
  }

  _pf_sampler_table.fill(updated);
  return std::nullopt;
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_demand_access(champsim::address line_addr, uint64_t cycle, champsim::address demand_pc)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_noignite_masking::make_line_tag(line_addr);

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  pf_sampler_entry entry = hit.value();

  // A Trained shadow has already had its one positive training event. The check_hit
  // above refreshed its LRU recency, which is the whole effect of this access: the
  // demand metadata backing the original event stays frozen, and no event is emitted.
  if (entry.training == TrainingState::Trained)
  {
    return std::nullopt;
  }

  entry.demand_pc = demand_pc;

  if (delay_subtimely_training)
  {
    entry.demand_timestamp = cycle;

    if (!entry.in_cache)
    {
      // This is a late prefetch: keep the entry until cache_fill computes latency.
      entry.training = TrainingState::AwaitingFill;
      _pf_sampler_table.fill(entry);
      return std::nullopt;
    }
  }
  else
  {
    entry.in_cache = true;
  }

  // The demand consumed a timely prefetch. Emit the single CacheHit event, but keep
  // the entry as a Trained shadow: its line is still in cache, so it must still be
  // attributed to whichever of the sampler or the cache replaces it first.
  entry.training = TrainingState::Trained;
  _pf_sampler_table.fill(entry);

  _cache_hit_events++;
  return EvictionEvent{entry, EvictionCause::CacheHit};
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_cache_evict(champsim::address line_addr)
{
  pf_sampler_entry probe{};
  probe.line_tag = pumpkin_noignite_masking::make_line_tag(line_addr);

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  pf_sampler_entry entry = hit.value();

  // Only issued prefetches that reached the cache are contesting the race.
  if (!entry.in_calibration_race())
  {
    return std::nullopt;
  }

  // The cache dropped the line while the sampler still tracked it, so the race
  // resolves cache-first. Counted once for unused live prefetches and for Trained
  // shadows alike.
  report_cache_evict();

  // Bookkeeping
  entry.in_cache = false;
  entry.calibration_live = false;

  _pf_sampler_table.invalidate(probe);

  // A shadow was already trained positively, so the calibration count above is its
  // only remaining contribution; only an unused prefetch is trained negatively.
  if (entry.training != TrainingState::Pending)
  {
    return std::nullopt;
  }

  return EvictionEvent{entry, EvictionCause::CacheEvict};
}

} // namespace pumpkin_noignite_detail
