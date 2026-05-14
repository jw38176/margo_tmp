#ifndef PUMPKIN_SAMPLER_H
#define PUMPKIN_SAMPLER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "address.h"
#include "msl/lru_table.h"

class Sampler
{
public:
  explicit Sampler(std::size_t sampler_sets, std::size_t sampler_ways);
  ~Sampler();

  void init(std::size_t num_prefetchers);

  // Indicates why a sampler_entry is sent to the controller, which uses it for training:
  //  - SamplerCapacity: LRU replacement, treated as a cache eviction.
  //  - CacheEvict: Real prefetched line evicted from cache before use (negative training).
  //  - CacheHit: Demand access consumed a prefetched line (positive training).
  enum class EvictionCause
  {
    SamplerCapacity,
    CacheEvict,
    CacheHit,
  };

  struct sampler_entry
  {
    // Prefetch Metadata
    champsim::address pf_line_addr;
    uint64_t fill_latency{0}; // Time taken from prefetch request to cache fill.
    uint64_t first_used_timestamp{0};
    std::vector<uint64_t> prefetch_timestamps;       // Length == _num_prefetchers
    std::vector<champsim::address> prefetch_src_pcs; // Length == _num_prefetchers

    // Prefetch Life Cycle Flags
    bool prefetched{false}; // Whether the prefetch was issued or filtered
    bool in_cache{false};   // Mirrors the cache state of pf_line_addr. Always false for a filtered entry.
    bool useful{false};     // Set after positive training has fired for this entry, prevents double-credit on a subsequent eviction event.
    bool valid{true};       // Cleared after any terminal event so subsequent cache/sampler evicts don't double-count

    auto index() const
    {
      using namespace champsim::data::data_literals;
      return pf_line_addr.slice_upper<2_b>();
    }
    auto tag() const
    {
      using namespace champsim::data::data_literals;
      return pf_line_addr.slice_upper<2_b>();
    }
  };

  struct EvictionEvent
  {
    sampler_entry entry;
    EvictionCause cause;
  };

  bool should_sample(); // Frequency gated by _train_ratio

  // Records a prefetch candidate. If inserting evicts an entry, returns a SamplerCapacity event; otherwise returns std::nullopt.
  std::optional<EvictionEvent> record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc, std::size_t prefetcher_id,
                                               bool prefetched); // `prefetched` is false for a filtered prefetch request

  // Updates sampler entry when its prefetched line is filled into cache.
  void notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle);

  // On demand cache access, returns CacheHit event and invalidates matching sampler entry.
  std::optional<EvictionEvent> notify_demand_access(champsim::address line_addr, uint64_t cycle);

  // On cache eviction, returns CacheEvict event for real prefetches that were still tracked.
  std::optional<EvictionEvent> notify_cache_evict(champsim::address line_addr);

private:
  std::size_t _num_prefetchers{0};

  std::size_t _sampler_sets;
  std::size_t _sampler_ways;
  static constexpr uint64_t MAX_TRAIN_RATIO = 1024;
  static constexpr uint64_t MIN_TRAIN_RATIO = 2;
  static constexpr uint64_t RETUNE_MIN_EVENTS = 400; // Minimum terminal events to trigger retuning
  static constexpr uint64_t RETUNE_IMBALANCE = 2;    // Imbalance factor between sampler and cache evictions

  uint64_t _train_ratio{MIN_TRAIN_RATIO};
  uint64_t _rng_state{0};
  uint64_t rand_u64();

  // Calibrate to L2 capacity
  uint64_t _cache_evicts{0};
  uint64_t _table_evicts{0};
  void maybe_retune_train_ratio();

  champsim::msl::lru_table<sampler_entry> _table{_sampler_sets, _sampler_ways};
};

#endif
