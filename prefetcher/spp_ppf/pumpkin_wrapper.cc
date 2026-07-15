#include "pumpkin_wrapper.h"

#include <memory>
#include <utility>
#include <vector>

#include "cache.h"
#include "spp_ppf.h"

namespace
{

class SppPpfPumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit SppPpfPumpkinWrapper(CACHE* cache) : _prefetcher(cache), _cache(cache) {}

  void initialize() final { _prefetcher.prefetcher_initialize(); }

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t /*cache_hit*/, bool /*useful_prefetch*/,
                                                        access_type type, uint32_t metadata_in) final
  {
    if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
      return {metadata_in, {}};
    }

    auto requests = _prefetcher.generate(addr.to<uint64_t>(), ip.to<uint64_t>(), static_cast<uint32_t>(_cache->get_pq_occupancy().back()),
                                         static_cast<uint32_t>(_cache->get_pq_size().back()), static_cast<uint32_t>(_cache->get_mshr_occupancy()),
                                         static_cast<uint32_t>(_cache->get_mshr_size()), _cache->warmup);

    std::vector<pumpkin_wrappers::WrappedPrefetch> candidates;
    candidates.reserve(requests.size());
    for (const auto& req : requests) {
      candidates.push_back({champsim::address{req.pf_addr}, ip, req.fill_this_level, req.metadata, [this, req](bool issued, bool filtered_out) {
                              if (issued) {
                                _prefetcher.on_issue(req);
                              } else if (!filtered_out) {
                                _prefetcher.prefetch_q_full++;
                              }
                            }});
    }

    return {metadata_in, std::move(candidates)};
  }

  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address /*addr*/, long /*set*/, long /*way*/, uint8_t /*prefetch*/,
                                                     champsim::address evicted_addr, uint32_t metadata_in) final
  {
    if (evicted_addr != champsim::address{}) {
      _prefetcher.notify_fill_evict(evicted_addr.to<uint64_t>());
    }
    return {metadata_in, {}};
  }

private:
  spp_ppf _prefetcher;
  CACHE* _cache;
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_spp_ppf_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<SppPpfPumpkinWrapper>(cache);
}
