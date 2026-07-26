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
    uint64_t line_tag{0};                             // Hashed, truncated tag of the prefetched line (storage-efficient)
    uint64_t fill_latency{0};                         // Time taken from prefetch request to cache fill 
    uint64_t first_request_timestamp{0};              // Masked timestamp of the first prefetch request for the line
    uint64_t demand_timestamp{0};                     // Masked timestamp of the demand access that consumed the prefetched line
    champsim::address demand_pc{};                    // PC of the demand access that consumed the prefetched line
    std::vector<uint64_t> prefetch_timestamps;        // Length == _num_prefetchers (masked timestamps)
    std::vector<champsim::address> prefetch_src_pcs;  // Length == _num_prefetchers

    // Prefetch Life Cycle Flags
    bool prefetched{false}; // Whether the prefetch was issued or filtered (currently only for tracking)
    bool in_cache{false};   // Mirrors the cache state of the line. Always false for a filtered entry.
    bool useful{false};     // Set after demand consumes this entry, prevents negative training on a subsequent eviction event.
    bool valid{true};       // Cleared after any terminal event so subsequent cache/pf_sampler evicts don't double-count

    // Ground-truth fill origin, captured at cache-fill time. from_dram == true
    // means the fill was served by DRAM; from_dram == false means it was served
    // by a lower-level cache (the LLC). Only meaningful once fill_latency != 0.
    bool from_dram{false};

    // LLC residency probed eagerly at record time (when the full address is still
    // available). Used by the controller to approximate the fill origin for lines
    // that never actually filled (fill_latency == 0), since the truncated tag can
    // no longer probe the LLC.
    bool llc_resident_at_record{false};

    auto index() const { return line_tag; }
    auto tag() const { return line_tag; }
  };

  struct EvictionEvent
  {
    pf_sampler_entry entry;
    EvictionCause cause;
  };

  // Records a prefetch candidate. If inserting evicts an entry, returns a PfSamplerCapacity event; otherwise returns std::nullopt.
  // `llc_resident` is the LLC residency of the line probed eagerly at record time.
  std::optional<EvictionEvent> record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc, std::size_t prefetcher_id,
                                               bool prefetched, bool llc_resident); // `prefetched` is false for a filtered prefetch request

  // Updates pf_sampler entry when its prefetched line is filled into cache.
  // `from_dram` is the cache's ground-truth fill origin for this line (true if
  // served by DRAM, false if served by the LLC).
  std::optional<PfSampler::EvictionEvent> notify_cache_fill(champsim::address pf_line_addr, uint64_t cycle, bool from_dram);

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
