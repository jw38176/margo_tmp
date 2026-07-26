#ifndef PREFETCHER_SUB_PREFETCHER_WRAPPED_PREFETCHER_H
#define PREFETCHER_SUB_PREFETCHER_WRAPPED_PREFETCHER_H

#include <cstdint>
#include <functional>
#include <vector>

#include "access_type.h"
#include "address.h"

class CACHE;

namespace pumpkin_wrappers
{

struct WrappedPrefetch
{
  champsim::address address{};
  champsim::address trigger_ip{};
  bool fill_this_level = true;
  uint32_t metadata = 0;
  std::function<void(bool issued, bool filtered_out)> notify_result{};
};

struct WrappedPrefetchResult
{
  uint32_t metadata = 0;
  std::vector<WrappedPrefetch> prefetches;
};

class WrappedPrefetcher
{
public:
  virtual ~WrappedPrefetcher() = default;

  virtual void initialize() {}

  virtual WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                              uint32_t metadata_in) = 0;

  virtual WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                           uint32_t metadata_in) = 0;

  virtual std::vector<WrappedPrefetch> cycle_operate() { return {}; }

  virtual void final_stats() {}

  // Optional hook: the meta-prefetcher forwards the LLC reference (discovered by the simulator) so a
  // wrapped sub-prefetcher can drive LLC way partitioning. Default is a no-op.
  virtual void set_llc_reference(CACHE* llc) { (void)llc; }

  // Optional hook: the meta-prefetcher installs a predicate deciding whether the wrapped
  // sub-prefetcher should be trained (told about) an access at a given PC. `is_miss` indicates the
  // access was a demand miss. Default is a no-op (sub-prefetcher trains on everything).
  virtual void set_training_gate(std::function<bool(champsim::address ip, bool is_miss)> gate) { (void)gate; }
};

} // namespace pumpkin_wrappers

#endif
