#ifndef PUMPKIN_PF_SAMPLER_H
#define PUMPKIN_PF_SAMPLER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "../_base_sampler/sampler.hh"
#include "address.h"
#include "msl/lru_table.h"

class PfSampler : public Sampler
{
public:
  // Set to true to defer positive training for late prefetches until cache fill
  // computes fill latency; set to false for immediate training on demand access.
  bool delay_subtimely_training = true;

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
    LatePrefetch,
  };

  struct pf_sampler_entry
  {
    // Prefetch Metadata
    champsim::address pf_line_addr;
    uint64_t fill_latency{0};                         // Time taken from prefetch request to cache fill 
    uint64_t first_request_timestamp{0};              // Timestamp of the first prefetch request for the pf_line_addr
    uint64_t demand_timestamp{0};                     // Timestamp of the demand access that consumed the prefetched line
    champsim::address demand_pc{};                    // PC of the demand access that consumed the prefetched line
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
      return pf_line_addr.slice_upper<6_b>();
    }
    auto tag() const
    {
      using namespace champsim::data::data_literals;
      return pf_line_addr.slice_upper<6_b>();
    }
  };

  struct EvictionEvent
  {
    pf_sampler_entry entry;
    EvictionCause cause;
  };

  // Records a prefetch candidate. If inserting evicts an entry, returns a PfSamplerCapacity event; otherwise returns std::nullopt.
  std::optional<EvictionEvent> record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc, std::size_t prefetcher_id,
                                               bool prefetched); // `prefetched` is false for a filtered prefetch request

  // Updates pf_sampler entry when its prefetched line is filled into cache.
  std::optional<PfSampler::EvictionEvent> notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle);

  // On demand cache access, returns CacheHit event and invalidates matching pf_sampler entry.
  std::optional<EvictionEvent> notify_demand_access(champsim::address line_addr, uint64_t cycle, champsim::address demand_pc);

  // On cache eviction, returns CacheEvict event for real prefetches that were still tracked.
  std::optional<EvictionEvent> notify_cache_evict(champsim::address line_addr);

  uint64_t cache_hit_events() const { return _cache_hit_events; }
  uint64_t late_prefetch_events() const { return _late_prefetch_events; }

private:
  std::size_t _num_prefetchers{0};

  std::size_t _pf_sampler_sets;
  std::size_t _pf_sampler_ways;

  champsim::msl::lru_table<pf_sampler_entry> _pf_sampler_table{_pf_sampler_sets, _pf_sampler_ways};

  uint64_t _cache_hit_events{0};
  uint64_t _late_prefetch_events{0};
};

#endif
