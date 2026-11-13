#include "margo_wrapper.h"

#include <utility>

#include "cache.h"
#include "pythia.h"
#include "pythia_params.h"

namespace
{

class PythiaMargoWrapper final : public margo_wrappers::WrappedPrefetcher
{
public:
  explicit PythiaMargoWrapper(CACHE* cache) : _cache(cache) {}

  void initialize() final { _prefetcher.initialize(); }

  margo_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
      return {metadata_in, {}};
    }

    std::vector<margo_wrappers::WrappedPrefetch> candidates;
    for (auto pf_addr : _prefetcher.prefetch_candidates(addr.to<uint64_t>(), ip.to<uint64_t>(), get_dram_bw())) {
      candidates.push_back({champsim::address{pf_addr}, ip, true, metadata_in, {}});
    }

    return {metadata_in, std::move(candidates)};
  }

  margo_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                     uint32_t metadata_in) final
  {
    _prefetcher.register_fill(addr.to<uint64_t>());
    return {metadata_in, {}};
  }

private:
  PythiaPrefetcher _prefetcher;
  CACHE* _cache;
};

} // namespace

std::unique_ptr<margo_wrappers::WrappedPrefetcher> make_pythia_margo_wrapper(CACHE* cache)
{
  return std::make_unique<PythiaMargoWrapper>(cache);
}
