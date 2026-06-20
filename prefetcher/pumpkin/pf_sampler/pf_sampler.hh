#ifndef PUMPKIN_PF_SAMPLER_H
#define PUMPKIN_PF_SAMPLER_H

// Set to 1 to defer positive training for late prefetches until cache fill
// computes fill latency; set to 0 for immediate training on demand access.
#define PUMPKIN_DELAYED_LATE_PREFETCHES 0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "address.h"
#include "msl/lru_table.h"

class PfSampler
{
public:
  explicit PfSampler(std::size_t pf_sampler_sets, std::size_t pf_sampler_ways);
  ~PfSampler();

  void init(std::size_t num_prefetchers);

  // Indicates why a pf_sampler_entry is sent to the controller, which uses it for training:
  //  - PfSamplerCapacity: LRU replacement, treated as a cache eviction.
  //  - CacheEvict: Real prefetched line evicted from cache before use (negative training).
  //  - CacheHit: Demand access consumed a prefetched line (positive training).
  enum class EvictionCause
  {
    PfSamplerCapacity,
    CacheEvict,
    CacheHit,
  };

  struct pf_sampler_entry
  {
    // Prefetch Metadata
    champsim::address pf_line_addr;
    uint64_t fill_latency{0};                         // Time taken from prefetch request to cache fill 
    uint64_t first_request_timestamp{0};              // Timestamp of the first prefetch request for the pf_line_addr
    uint64_t demand_timestamp{0};                     // Timestamp of the demand access that consumed the prefetched line
    std::vector<uint64_t> prefetch_timestamps;        // Length == _num_prefetchers
    std::vector<champsim::address> prefetch_src_pcs;  // Length == _num_prefetchers

    // Prefetch Life Cycle Flags
    bool prefetched{false}; // Whether the prefetch was issued or filtered (currently only for tracking)
    bool in_cache{false};   // Mirrors the cache state of pf_line_addr. Always false for a filtered entry.
    bool useful{false};     // Set after demand consumes this entry, prevents negative training on a subsequent eviction event.
    bool valid{true};       // Cleared after any terminal event so subsequent cache/pf_sampler evicts don't double-count

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
    pf_sampler_entry entry;
    EvictionCause cause;
  };

  bool should_sample(champsim::address pf_line_addr) const; // Address-gated by _train_ratio

  // Records a prefetch candidate. If inserting evicts an entry, returns a PfSamplerCapacity event; otherwise returns std::nullopt.
  std::optional<EvictionEvent> record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc, std::size_t prefetcher_id,
                                               bool prefetched); // `prefetched` is false for a filtered prefetch request

#if PUMPKIN_DELAYED_LATE_PREFETCHES
  // Updates pf_sampler entry when its prefetched line is filled into cache. If the
  // line was already demanded while outstanding, returns a delayed CacheHit event.
  std::optional<EvictionEvent> notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle);
#else
  // Updates pf_sampler entry when its prefetched line is filled into cache.
  void notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle);
#endif

  // On demand cache access, returns CacheHit event and invalidates matching pf_sampler entry.
  std::optional<EvictionEvent> notify_demand_access(champsim::address line_addr, uint64_t cycle);

  // On cache eviction, returns CacheEvict event for real prefetches that were still tracked.
  std::optional<EvictionEvent> notify_cache_evict(champsim::address line_addr);

private:
  std::size_t _num_prefetchers{0};

  std::size_t _pf_sampler_sets;
  std::size_t _pf_sampler_ways;
  static constexpr uint64_t MAX_TRAIN_RATIO = 1024;
  static constexpr uint64_t MIN_TRAIN_RATIO = 2;
  static constexpr uint64_t RETUNE_MIN_EVENTS = 400; // Minimum terminal events to tr`igger retuning
  static constexpr uint64_t RETUNE_IMBALANCE = 2;    // Imbalance factor between pf_sampler and cache evictions

  uint64_t _train_ratio{MIN_TRAIN_RATIO};

  // Calibrate to L2 capacity
  uint64_t _cache_evicts{0};
  uint64_t _table_evicts{0};
  void maybe_retune_train_ratio();

  champsim::msl::lru_table<pf_sampler_entry> _pf_sampler_table{_pf_sampler_sets, _pf_sampler_ways};
};

#endif
