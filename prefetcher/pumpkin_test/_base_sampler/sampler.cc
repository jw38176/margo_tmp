#include "sampler.hh"

namespace pumpkin_test_detail
{


void Sampler::init()
{
  _train_ratio = MIN_TRAIN_RATIO;
  _cache_evicts = 0;
  _table_evicts = 0;

  _peak_train_ratio = MIN_TRAIN_RATIO;
  _retune_up = 0;
  _retune_down = 0;
  _table_evicts_total = 0;
  _cache_evicts_total = 0;
}

Sampler::SamplerStats Sampler::stats() const
{
  return SamplerStats{_train_ratio, MIN_TRAIN_RATIO,     MAX_TRAIN_RATIO,     _peak_train_ratio,
                      _retune_up,   _retune_down,         _table_evicts_total, _cache_evicts_total};
}

uint64_t Sampler::hash_line_addr(champsim::address line_addr)
{
  uint64_t x = line_addr.to<uint64_t>();
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

bool Sampler::should_sample(champsim::address line_addr) const
{
  if (_train_ratio <= 1)
    return true;

  return (hash_line_addr(line_addr) % _train_ratio) == 0;
}

void Sampler::report_sampler_table_evict()
{
  _table_evicts++;
  _table_evicts_total++;
  maybe_retune_train_ratio();
}

void Sampler::report_cache_evict()
{
  _cache_evicts++;
  _cache_evicts_total++;
  maybe_retune_train_ratio();
}

void Sampler::maybe_retune_train_ratio()
{

  if (_table_evicts > RETUNE_MIN_EVENTS && _table_evicts > _cache_evicts * RETUNE_IMBALANCE && _train_ratio < MAX_TRAIN_RATIO)
  {
    _train_ratio *= 2;
    _retune_up++;
    if (_train_ratio > _peak_train_ratio)
      _peak_train_ratio = _train_ratio;
    _cache_evicts = 0;
    _table_evicts = 0;
    return;
  }

  if (_cache_evicts > RETUNE_MIN_EVENTS && _cache_evicts > _table_evicts * RETUNE_IMBALANCE && _train_ratio > MIN_TRAIN_RATIO)
  {
    _train_ratio /= 2;
    _retune_down++;
    _cache_evicts = 0;
    _table_evicts = 0;
  }
}

} // namespace pumpkin_test_detail
