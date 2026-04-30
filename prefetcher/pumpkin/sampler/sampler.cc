#include "sampler.hh"

Sampler::Sampler(std::size_t sampler_sets, std::size_t sampler_ways): 
_sampler_sets(sampler_sets), 
_sampler_ways(sampler_ways) 
{}

Sampler::~Sampler() = default;

void Sampler::init(std::size_t num_prefetchers)
{
  _train_ratio = MIN_TRAIN_RATIO;
  _rng_state = 0x9E3779B97F4A7C15ULL;
  _num_prefetchers = num_prefetchers;
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
  if (_train_ratio <= 1) {
    return true;
  }
  return (rand_u64() % _train_ratio) == 0;
}

std::optional<Sampler::sampler_entry> Sampler::record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_ip, std::size_t prefetcher_id)
{
  sampler_entry probe{};
  probe.pf_line_addr = pf_line_addr;

  // Check if the prefetch is already in the table
  if (auto hit = _table.check_hit(probe); hit.has_value()) {
    sampler_entry updated = hit.value();
    if (prefetcher_id < updated.prefetch_timestamps.size()) {
      updated.prefetch_timestamps[prefetcher_id] = cycle;
      updated.prefetch_src_pcs[prefetcher_id] = src_ip;
    }
    // Update the entry in the table
    _table.fill(updated);
    return std::nullopt;
  }

  // Otherwise, build a fresh entry for this line.
  sampler_entry e{};
  e.pf_line_addr = pf_line_addr;
  e.fill_latency = 0;
  e.first_used_timestamp = cycle;
  e.prefetch_timestamps.assign(_num_prefetchers, 0);
  e.prefetch_src_pcs.assign(_num_prefetchers, champsim::address{});
  if (prefetcher_id < _num_prefetchers) {
    e.prefetch_timestamps[prefetcher_id] = cycle;
    e.prefetch_src_pcs[prefetcher_id] = src_ip;
  }

  // Insert the new entry; fill() returns the LRU entry it replaced, or
  // std::nullopt if it landed in a previously-invalid way.
  return _table.fill(e);
}

void Sampler::mark_filled(champsim::address pf_line_addr, uint64_t cycle)
{
  sampler_entry probe{};
  probe.pf_line_addr = pf_line_addr;

  auto hit = _table.check_hit(probe);
  if (!hit.has_value()) {
    return;
  }

  sampler_entry updated = hit.value();
  if (updated.fill_latency == 0 && cycle >= updated.first_used_timestamp) {
    updated.fill_latency = cycle - updated.first_used_timestamp;
  }
  _table.fill(updated);
}

std::optional<Sampler::sampler_entry>
Sampler::check_and_evict_entry(champsim::address line_addr)
{
  sampler_entry probe{};
  probe.pf_line_addr = line_addr;

  auto removed = _table.invalidate(probe);
  return removed;
}

void Sampler::retune_train_ratio(uint64_t cache_evicts, uint64_t table_evicts)
{
  // Sampler is thrashing faster than the cache: track fewer prefetches.
  if (table_evicts > 400 && table_evicts > cache_evicts * 2
      && _train_ratio < MAX_TRAIN_RATIO) {
    _train_ratio *= 2;
    return;
  }

  // Cache is churning much faster than the sampler: we can afford to track
  // a denser sample.
  if (cache_evicts > 400 && cache_evicts > table_evicts * 2
      && _train_ratio > MIN_TRAIN_RATIO) {
    _train_ratio /= 2;
  }
}
