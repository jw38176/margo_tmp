#include "pumpkin_wrapper.h"

#include "cache.h"
#include "ip_stride.h"

namespace
{

class IpStridePumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit IpStridePumpkinWrapper(CACHE* cache) : _prefetcher(cache), _cache(cache) {}

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    const uint32_t metadata = _prefetcher.prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
    if (_prefetcher.active_lookahead.has_value()) {
      _active_ip = ip;
    }
    return {metadata, {}};
  }

  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                     uint32_t metadata_in) final
  {
    const uint32_t metadata = _prefetcher.prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
    return {metadata, {}};
  }

  std::vector<pumpkin_wrappers::WrappedPrefetch> cycle_operate() final
  {
    if (!_prefetcher.active_lookahead.has_value()) {
      return {};
    }

    const auto lookahead = _prefetcher.active_lookahead.value();
    const auto old_pf_address = lookahead.address;
    const auto stride = lookahead.stride;
    const auto degree = lookahead.degree;
    champsim::address pf_address{champsim::block_number{old_pf_address} + stride};

    if (!_cache->virtual_prefetch && champsim::page_number{pf_address} != champsim::page_number{old_pf_address}) {
      _prefetcher.active_lookahead.reset();
      return {};
    }

    const bool fill_this_level = _cache->get_mshr_occupancy_ratio() < 0.5;
    return {{pf_address, _active_ip, fill_this_level, 0,
             [this, pf_address, stride, degree](bool issued, bool filtered_out) {
               if (issued || filtered_out) {
                 _prefetcher.active_lookahead = {pf_address, stride, degree - 1};
                 if (_prefetcher.active_lookahead->degree == 0) {
                   _prefetcher.active_lookahead.reset();
                 }
               }
             }}};
  }

private:
  ip_stride _prefetcher;
  CACHE* _cache;
  champsim::address _active_ip{};
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_ip_stride_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<IpStridePumpkinWrapper>(cache);
}
