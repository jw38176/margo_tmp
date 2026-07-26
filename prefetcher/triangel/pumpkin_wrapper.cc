#include "pumpkin_wrapper.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "triangel.h"

namespace
{

class TriangelPumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit TriangelPumpkinWrapper(CACHE* cache) : _prefetcher(cache) {}

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    // The meta-prefetcher decides whether Triangel should be trained on (told about) this access.
    const bool is_miss = (cache_hit == 0);
    const bool allow_training = !_training_gate || _training_gate(ip, is_miss);

    // Filtered inputs are still shown to Triangel, but as a store, so the set dueller's
    // L3/cache-side accounting (independent of filtering) is preserved while training and
    // Markov-side credit are withheld.
    const access_type effective_type = allow_training ? type : access_type::WRITE;

    // Unlike most sub-prefetchers, triangel also consumes WRITE accesses (for its cache-side set
    // dueling), so all access types are forwarded and filtered inside prefetch_candidates().
    std::vector<pumpkin_wrappers::WrappedPrefetch> candidates;
    for (auto& candidate : _prefetcher.prefetch_candidates(addr, ip, cache_hit, useful_prefetch, effective_type, metadata_in, allow_training)) {
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

  std::vector<pumpkin_wrappers::WrappedPrefetch> cycle_operate() final
  {
    _prefetcher.prefetcher_cycle_operate();
    return {};
  }

  void final_stats() final { _prefetcher.prefetcher_final_stats(); }

  void set_llc_reference(CACHE* llc) final { _prefetcher.set_llc_reference(llc); }

  void set_training_gate(std::function<bool(champsim::address ip, bool is_miss)> gate) final { _training_gate = std::move(gate); }

private:
  triangel _prefetcher;
  std::function<bool(champsim::address ip, bool is_miss)> _training_gate{};
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_triangel_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<TriangelPumpkinWrapper>(cache);
}
