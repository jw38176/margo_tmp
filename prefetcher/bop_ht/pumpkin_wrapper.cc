#include "pumpkin_wrapper.h"

#include <memory>

#include "bop_ht.h"
#include "cache.h"

namespace
{

class BopHtPumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit BopHtPumpkinWrapper(CACHE* cache) : _prefetcher(cache), _cache(cache) {}

  void initialize() final { _prefetcher.prefetcher_initialize(); }

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    auto pf_addr = _prefetcher.operate(addr, cache_hit, useful_prefetch, type);
    if (!pf_addr.has_value()) {
      return {metadata_in, {}};
    }

    const champsim::address pf = *pf_addr;
    const bool base_cache_hit = cache_hit != 0;
    return {metadata_in, {{pf, ip, true, 0x1, [this, pf, addr, base_cache_hit](bool issued, bool filtered_out) {
                             // The meta-prefetcher checks residency before issuing, so an unissued,
                             // unfiltered prefetch may target a line that will never fill.
                             const bool pf_in_cache = !filtered_out && !issued && _cache->in_cache(pf);
                             _prefetcher.prefetchOutcome(pf.to<uint64_t>(), addr.to<uint64_t>(), base_cache_hit, filtered_out, pf_in_cache);
                           }}}};
  }

  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long, long, uint8_t, champsim::address, uint32_t metadata_in) final
  {
    _prefetcher.insertFill(addr.to<uint64_t>());
    return {metadata_in, {}};
  }

private:
  bop_ht _prefetcher;
  CACHE* _cache;
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_bop_ht_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<BopHtPumpkinWrapper>(cache);
}
