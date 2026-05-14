#include "sampler.hh"

Sampler::Sampler(std::size_t sampler_sets, std::size_t sampler_ways) : _sampler_sets(sampler_sets), _sampler_ways(sampler_ways) {}

Sampler::~Sampler() = default;

void Sampler::init(std::size_t num_prefetchers)
{
  _train_ratio = MIN_TRAIN_RATIO;
  _rng_state = 0x9E3779B97F4A7C15ULL;
  _num_prefetchers = num_prefetchers;
  _cache_evicts = 0;
  _table_evicts = 0;
}

uint64_t Sampler::rand_u64()
{
  uint64_t x = _rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  _rng_state = x;
  return x;
}

bool Sampler::should_sample()
{
  if (_train_ratio <= 1)
    return true;

  return (rand_u64() % _train_ratio) == 0;
}

std::optional<Sampler::EvictionEvent> Sampler::record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc,
                                                               std::size_t prefetcher_id, bool prefetched)
{
  sampler_entry probe{};
  probe.pf_line_addr = pf_line_addr;

  // If a sampler entry already exists for this line, merge into it rather than overwriting.
  if (auto hit = _table.check_hit(probe); hit.has_value())
  {
    sampler_entry updated = hit.value();

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
      _table.fill(updated);
      return std::nullopt;
    }
    // useful==true means valid==false and we've already trained for this entry.
    // Overwrite it with a fresh entry below.
  }

  sampler_entry e{};
  e.pf_line_addr = pf_line_addr;
  e.fill_latency = 0;
  e.first_used_timestamp = cycle;
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

  auto evicted = _table.fill(e);
  if (!evicted.has_value())
  {
    return std::nullopt;
  }

  sampler_entry& victim = evicted.value();

  // Only calibrate on terminal events for prefetched entries that were filled into cache.
  if (victim.prefetched && victim.in_cache && victim.valid && !victim.useful)
  {
    _table_evicts++;
    maybe_retune_train_ratio();
  }

  if (!victim.valid)
  {
    // The entry has already had its terminal event; suppress the duplicate.
    return std::nullopt;
  }

  victim.valid = false;
  return EvictionEvent{victim, EvictionCause::SamplerCapacity};
}

void Sampler::notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle)
{
  sampler_entry probe{};
  probe.pf_line_addr = pf_line_addr;

  auto hit = _table.check_hit(probe);
  if (!hit.has_value())
    return;

  sampler_entry updated = hit.value();

  if (updated.fill_latency == 0 && cycle >= updated.first_used_timestamp)
  {
    updated.fill_latency = cycle - updated.first_used_timestamp;
  }
  updated.in_cache = true;

  _table.fill(updated);
}

std::optional<Sampler::EvictionEvent> Sampler::notify_demand_access(champsim::address line_addr, uint64_t /*cycle*/)
{
  sampler_entry probe{};
  probe.pf_line_addr = line_addr;

  auto hit = _table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  sampler_entry entry = hit.value();
  if (!entry.valid)
  {
    return std::nullopt;
  }

  // Mark entry as used by demand to prevent double-counting.
  entry.useful = true;
  entry.valid = false;
  entry.in_cache = true;

  // Snapshot the consumed entry for the controller, then invalidate the
  // sampler slot so the LRU can reclaim it without producing an extra
  // SamplerCapacity event.
  sampler_entry snapshot = entry;
  _table.invalidate(probe);

  return EvictionEvent{snapshot, EvictionCause::CacheHit};
}

std::optional<Sampler::EvictionEvent> Sampler::notify_cache_evict(champsim::address line_addr)
{
  sampler_entry probe{};
  probe.pf_line_addr = line_addr;

  auto hit = _table.check_hit(probe);
  if (!hit.has_value())
    return std::nullopt;

  sampler_entry entry = hit.value();

  // Only valid, filled prefetches generate cache-eviction events.
  if (!entry.prefetched || !entry.in_cache || !entry.valid || entry.useful)
  {
    return std::nullopt;
  }

  entry.in_cache = false;
  entry.valid = false;

  _cache_evicts++;
  maybe_retune_train_ratio();

  _table.invalidate(probe);

  return EvictionEvent{entry, EvictionCause::CacheEvict};
}

void Sampler::maybe_retune_train_ratio()
{
  // Sampler is thrashing faster than the cache: track fewer prefetches.
  if (_table_evicts > RETUNE_MIN_EVENTS && _table_evicts > _cache_evicts * RETUNE_IMBALANCE && _train_ratio < MAX_TRAIN_RATIO)
  {
    _train_ratio *= 2;
    _cache_evicts = 0;
    _table_evicts = 0;
    return;
  }

  // Cache is churning much faster than the sampler: track a denser sample.
  if (_cache_evicts > RETUNE_MIN_EVENTS && _cache_evicts > _table_evicts * RETUNE_IMBALANCE && _train_ratio > MIN_TRAIN_RATIO)
  {
    _train_ratio /= 2;
    _cache_evicts = 0;
    _table_evicts = 0;
  }
}
