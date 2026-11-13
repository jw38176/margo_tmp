#include "margo_wrapper.h"

#include <memory>
#include <utility>

#include "pmp.h"

namespace
{

class PmpMargoWrapper final : public margo_wrappers::WrappedPrefetcher
{
public:
  explicit PmpMargoWrapper(CACHE* cache) : _prefetcher(cache) {}

  void initialize() final { _prefetcher.prefetcher_initialize(); }

  margo_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
      return {metadata_in, {}};
    }

    std::vector<margo_wrappers::WrappedPrefetch> candidates;
    for (auto& candidate : _prefetcher.prefetch_candidates(addr, ip, cache_hit, useful_prefetch, type, metadata_in)) {
      candidates.push_back({candidate.address, candidate.trigger_ip, candidate.fill_this_level, candidate.metadata, std::move(candidate.notify_result)});
    }

    return {metadata_in, std::move(candidates)};
  }

  margo_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                     uint32_t metadata_in) final
  {
    const uint32_t metadata = _prefetcher.prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
    return {metadata, {}};
  }

private:
  pmp _prefetcher;
};

} // namespace

std::unique_ptr<margo_wrappers::WrappedPrefetcher> make_pmp_margo_wrapper(CACHE* cache)
{
  return std::make_unique<PmpMargoWrapper>(cache);
}
