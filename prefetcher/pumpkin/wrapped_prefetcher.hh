#ifndef PREFETCHER_PUMPKIN_WRAPPED_PREFETCHER_H
#define PREFETCHER_PUMPKIN_WRAPPED_PREFETCHER_H

#include <cstdint>
#include <functional>
#include <vector>

#include "access_type.h"
#include "address.h"

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
};

} // namespace pumpkin_wrappers

#endif
