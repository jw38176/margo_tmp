#include "pumpkin_wrapper.h"

#include <memory>
#include <utility>

#include "berti.h"

namespace
{

class BertiPumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit BertiPumpkinWrapper(CACHE* cache) : _prefetcher(cache) {}

  void initialize() final { _prefetcher.prefetcher_initialize(); }

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
      return {metadata_in, {}};
    }

    std::vector<pumpkin_wrappers::WrappedPrefetch> candidates;
    for (auto& candidate : _prefetcher.prefetch_candidates(addr, ip, cache_hit, useful_prefetch, type, metadata_in)) {
      candidates.push_back({candidate.address, candidate.trigger_ip, candidate.fill_this_level, candidate.metadata, std::move(candidate.notify_result)});
    }

    return {metadata_in, std::move(candidates)};
  }

  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                     uint32_t metadata_in) final
  {
    const uint32_t metadata = _prefetcher.prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
    return {metadata, {}};
  }

private:
  berti _prefetcher;
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_berti_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<BertiPumpkinWrapper>(cache);
}
