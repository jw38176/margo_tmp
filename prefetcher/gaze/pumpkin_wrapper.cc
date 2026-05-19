#include "pumpkin_wrapper.h"

#include <iostream>
#include <utility>
#include <vector>

#include "cache.h"
#include "gaze.h"

namespace
{

class GazePumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit GazePumpkinWrapper(CACHE* cache)
      : _prefetcher(gaze_space::FT_SIZE, gaze_space::FT_WAY, gaze_space::AT_SIZE, gaze_space::AT_WAY, gaze_space::PT_SIZE, gaze_space::PT_WAY,
                    gaze_space::PB_SIZE, gaze_space::PB_WAY, cache->cpu),
        _cache(cache)
  {
  }

  void initialize() final { std::cout << _cache->NAME << " Gaze Pumpkin wrapper" << std::endl; }

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    _prefetcher.set_warmup(_cache->warmup);

    if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
      return {metadata_in, {}};
    }

    const uint64_t block_num = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
    _prefetcher.access(block_num, ip.to<uint64_t>(), _cache);

    std::vector<pumpkin_wrappers::WrappedPrefetch> candidates;
    for (const auto& candidate : _prefetcher.prefetch_candidates(_cache, block_num)) {
      candidates.push_back({champsim::address{candidate.address}, ip, candidate.fill_this_level, candidate.metadata, {}});
    }

    return {metadata_in, std::move(candidates)};
  }

  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                     uint32_t metadata_in) final
  {
    (void)addr;
    (void)set;
    (void)way;
    (void)prefetch;

    if (evicted_addr != champsim::address{}) {
      const uint64_t evicted_block_num = evicted_addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
      _prefetcher.eviction(evicted_block_num, _cache);
    }

    return {metadata_in, {}};
  }

private:
  gaze_space::Gaze _prefetcher;
  CACHE* _cache;
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_gaze_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<GazePumpkinWrapper>(cache);
}
