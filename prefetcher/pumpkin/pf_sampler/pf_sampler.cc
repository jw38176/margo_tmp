#include "pf_sampler.hh"

namespace
{
uint64_t hash_line_addr(champsim::address pf_line_addr)
{
  uint64_t x = pf_line_addr.to<uint64_t>();
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}
} // namespace

PfSampler::PfSampler(std::size_t pf_sampler_sets, std::size_t pf_sampler_ways) : _pf_sampler_sets(pf_sampler_sets), _pf_sampler_ways(pf_sampler_ways) {}

PfSampler::~PfSampler() = default;

void PfSampler::init(std::size_t num_prefetchers)
{
  _train_ratio = MIN_TRAIN_RATIO;
  _num_prefetchers = num_prefetchers;
  _cache_evicts = 0;
  _table_evicts = 0;
}

bool PfSampler::should_sample(champsim::address pf_line_addr) const
{
  if (_train_ratio <= 1)
    return true;

  return (hash_line_addr(pf_line_addr) % _train_ratio) == 0;
}

std::optional<PfSampler::EvictionEvent> PfSampler::record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc,
                                                               std::size_t prefetcher_id, bool prefetched)
{
  pf_sampler_entry probe{};
  probe.pf_line_addr = pf_line_addr;

  // If a pf_sampler entry already exists for this line, merge into it rather than overwriting.
  if (auto hit = _pf_sampler_table.check_hit(probe); hit.has_value())
  {
    pf_sampler_entry updated = hit.value();

    if (!updated.useful)
    {
      // Update mid-lifecycle entry with new contributor info.
      if (prefetcher_id < updated.prefetch_timestamps.size())
      {
        updated.prefetch_timestamps[prefetcher_id] = cycle;
        updated.prefetch_src_pcs[prefetcher_id] = src_pc;
      }
      if (prefetched)
      {
        updated.prefetched = true;
      }
      _pf_sampler_table.fill(updated);
      return std::nullopt;
    }

#if PUMPKIN_DELAYED_LATE_PREFETCHES
    if (updated.valid)
    {
      // A late prefetch was already consumed by demand and is waiting for fill
      // latency before positive training. Preserve that pending terminal event.
      return std::nullopt;
    }

    // useful==true and valid==false means we've already trained for this entry.
#else
    // useful==true means valid==false and we've already trained for this entry.
#endif
    // Overwrite it with a fresh entry below.
  }

  pf_sampler_entry e{};
  e.pf_line_addr = pf_line_addr;
  e.fill_latency = 0;
  e.first_request_timestamp = cycle;
#if PUMPKIN_DELAYED_LATE_PREFETCHES
  e.demand_timestamp = 0;
#endif
  e.prefetch_timestamps.assign(_num_prefetchers, 0);
  e.prefetch_src_pcs.assign(_num_prefetchers, champsim::address{});
  e.prefetched = prefetched;
  e.in_cache = false;
  e.useful = false;
  e.valid = true;

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
    _table_evicts++;
    maybe_retune_train_ratio();
  }

  if (!victim.valid || !victim.prefetched
#if PUMPKIN_DELAYED_LATE_PREFETCHES
      || victim.useful
#endif
  )
  {
    return std::nullopt;
  }

  victim.valid = false;
  return EvictionEvent{victim, EvictionCause::PfSamplerCapacity};
}

#if PUMPKIN_DELAYED_LATE_PREFETCHES
std::optional<PfSampler::EvictionEvent> PfSampler::notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle)
#else
void PfSampler::notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle)
#endif
{
  pf_sampler_entry probe{};
  probe.pf_line_addr = pf_line_addr;

  auto hit = _pf_sampler_table.check_hit(probe);
  if (!hit.has_value())
#if PUMPKIN_DELAYED_LATE_PREFETCHES
    return std::nullopt;
#else
    return;
#endif

  pf_sampler_entry updated = hit.value();

  // Calculate fill latency
  if (updated.fill_latency == 0 && cycle >= updated.first_request_timestamp)
  {
    updated.fill_latency = cycle - updated.first_request_timestamp;
  }

  updated.in_cache = true;

#if PUMPKIN_DELAYED_LATE_PREFETCHES
  // Delayed CacheHit event for late prefetches
  if (updated.useful && updated.valid)
  {
    updated.valid = false;
    pf_sampler_entry snapshot = updated;
    _pf_sampler_table.invalidate(probe);
    return EvictionEvent{snapshot, EvictionCause::CacheHit};
  }
#endif

  _pf_sampler_table.fill(updated);
#if PUMPKIN_DELAYED_LATE_PREFETCHES
  return std::nullopt;
#endif
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_demand_access(champsim::address line_addr, uint64_t cycle)
{
  pf_sampler_entry probe{};
  probe.pf_line_addr = line_addr;

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

#if PUMPKIN_DELAYED_LATE_PREFETCHES
  entry.demand_timestamp = cycle;

  if (!entry.in_cache)
  {
    // This is a late prefetch: keep the entry until cache_fill computes latency.
    _pf_sampler_table.fill(entry);
    return std::nullopt;
  }

  entry.valid = false;
#else
  entry.valid = false;
  entry.in_cache = true;
#endif

  // Snapshot the consumed entry for the controller, then invalidate the
  // pf_sampler slot so the LRU can reclaim it without producing an extra
  // PfSamplerCapacity event.
  pf_sampler_entry snapshot = entry;
  _pf_sampler_table.invalidate(probe);

  return EvictionEvent{snapshot, EvictionCause::CacheHit};
}

std::optional<PfSampler::EvictionEvent> PfSampler::notify_cache_evict(champsim::address line_addr)
{
  pf_sampler_entry probe{};
  probe.pf_line_addr = line_addr;

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

  _cache_evicts++;
  maybe_retune_train_ratio();

  _pf_sampler_table.invalidate(probe);

  return EvictionEvent{entry, EvictionCause::CacheEvict};
}

void PfSampler::maybe_retune_train_ratio()
{
  // PfSampler is thrashing faster than the cache: track fewer prefetches.
  if (_table_evicts > RETUNE_MIN_EVENTS && _table_evicts > _cache_evicts * RETUNE_IMBALANCE && _train_ratio < MAX_TRAIN_RATIO)
  {
    _train_ratio *= 2;
    _cache_evicts = 0;
    _table_evicts = 0;
    return;
  }

  // Cache is churning much faster than the pf_sampler: track a denser sample.
  if (_cache_evicts > RETUNE_MIN_EVENTS && _cache_evicts > _table_evicts * RETUNE_IMBALANCE && _train_ratio > MIN_TRAIN_RATIO)
  {
    _train_ratio /= 2;
    _cache_evicts = 0;
    _table_evicts = 0;
  }
}
