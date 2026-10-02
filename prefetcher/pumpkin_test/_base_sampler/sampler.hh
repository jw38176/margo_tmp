#ifndef PUMPKIN_TEST_SAMPLER_H
#define PUMPKIN_TEST_SAMPLER_H

#include <cstdint>

#include "address.h"

namespace pumpkin_test_detail
{


// Address-gated sampling rate for a pumpkin sampler. The gate uses a hash of the
// cache line address; train_ratio is retuned from this sampler's own
// sampler-table and cache eviction rates so its effective tracking density
// tracks L2C capacity. Each sampler instance owns its own gate, so samplers that
// track different events (e.g. prefetch utility vs criticality) size themselves
// independently rather than sharing a single ratio.
class Sampler
{
public:
  void init();

  bool should_sample(champsim::address line_addr) const;

  // Snapshot of the adaptive-gate state, used to judge whether the fixed-size
  // sampler table is large enough for the workload. The gate samples 1-in-
  // train_ratio lines and only sparsens (raises train_ratio above the floor)
  // when the table evicts live entries faster than the cache evicts lines, i.e.
  // when the table cannot keep up at the current density. So a train_ratio that
  // never leaves the floor means the table comfortably tracks the densest
  // allowed sample (large enough, with headroom); a train_ratio driven upward
  // means the working set outgrows the table and it auto-subsamples to cope.
  struct SamplerStats
  {
    uint64_t train_ratio;        // current (final) gate: sample 1-in-train_ratio lines
    uint64_t min_train_ratio;    // densest allowed sample (gate floor)
    uint64_t max_train_ratio;    // sparsest allowed sample (gate ceiling)
    uint64_t peak_train_ratio;   // highest train_ratio ever reached
    uint64_t retune_up;          // times the gate sparsened (table-pressure events)
    uint64_t retune_down;        // times the gate densified (table-headroom events)
    uint64_t table_evicts_total; // live sampler-table entries lost to capacity
    uint64_t cache_evicts_total; // tracked lines evicted from the cache
  };

  SamplerStats stats() const;

protected:
  void report_sampler_table_evict();
  void report_cache_evict();

  static uint64_t hash_line_addr(champsim::address line_addr);

private:
  static constexpr uint64_t MAX_TRAIN_RATIO = 1024;
  static constexpr uint64_t MIN_TRAIN_RATIO = 2;
  static constexpr uint64_t RETUNE_MIN_EVENTS = 400;
  static constexpr uint64_t RETUNE_IMBALANCE = 2;

  uint64_t _train_ratio{MIN_TRAIN_RATIO};
  uint64_t _cache_evicts{0};
  uint64_t _table_evicts{0};

  // Sizing diagnostics (do not feed back into the gate; reported at end of sim).
  uint64_t _peak_train_ratio{MIN_TRAIN_RATIO};
  uint64_t _retune_up{0};
  uint64_t _retune_down{0};
  uint64_t _table_evicts_total{0};
  uint64_t _cache_evicts_total{0};

  void maybe_retune_train_ratio();
};


} // namespace pumpkin_test_detail
#endif
