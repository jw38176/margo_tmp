#include "pumpkin_wrapper.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "cache.h"
#include "caerus.h"

namespace
{

class CaerusPumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit CaerusPumpkinWrapper(CACHE* cache) : _prefetcher(cache), _cache(cache) {}

  void initialize() final { _prefetcher.prefetcher_initialize(); }

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
      return {metadata_in, {}};
    }

    if (!cache_hit) {
      auto block_addr = champsim::block_number{addr};
      auto mshr_entry = std::find_if(_cache->MSHR.begin(), _cache->MSHR.end(),
                                     [block_addr](const auto& entry) { return champsim::block_number{entry.address} == block_addr; });
      if (mshr_entry != _cache->MSHR.end() && mshr_entry->type != access_type::PREFETCH) {
        return {metadata_in, {}};
      }
    }

    const uint64_t raw_addr = addr.to<uint64_t>();
    const uint64_t raw_ip = ip.to<uint64_t>();
    const uint64_t line_addr = raw_addr >> LOG2_BLOCK_SIZE;
    const uint64_t a_tag = _prefetcher.addr_tag(line_addr);

    if (_prefetcher.holding_table.test(a_tag) && cache_hit && useful_prefetch) {
      _prefetcher.holding_table.remove(a_tag);
    }

    if (!(cache_hit && useful_prefetch) && cache_hit) {
      return {metadata_in, {}};
    }

    auto pf_addrs = _prefetcher.calculateAccuratePrefetchAddrs(raw_addr, line_addr, raw_ip);
    bool added_to_holding = false;

    if (!_prefetcher.rr_table.isSamePage(_prefetcher.addr_tag(line_addr))) {
      _prefetcher.rr_insert_and_train_acc(line_addr, raw_ip);
      _prefetcher.subtimely_rr_insertion_counter++;
      added_to_holding = true;
    }

    std::vector<pumpkin_wrappers::WrappedPrefetch> candidates;
    auto group_added_to_holding = std::make_shared<bool>(added_to_holding);

    if (!pf_addrs.empty()) {
      for (auto pf_addr : pf_addrs) {
        if (pf_addr == 0) {
          continue;
        }

        const uint64_t pf_line_addr = pf_addr >> LOG2_BLOCK_SIZE;
        if (_prefetcher.holding_table.test(_prefetcher.addr_tag(pf_line_addr))) {
          continue;
        }

        if (_cache->get_pq_occupancy().back() < _cache->get_pq_size().back()
            && _cache->get_pq_occupancy().back() + _cache->get_mshr_occupancy() < _cache->get_mshr_size() - 1) {
          _prefetcher.below_threshold_counter++;
        }
        _prefetcher.general_pf_counter++;

        candidates.push_back({champsim::address{pf_addr}, ip, true, 0x1,
                              [this, group_added_to_holding, pf_line_addr, line_addr, raw_ip](bool issued, bool /*filtered_out*/) {
                                if (issued && !*group_added_to_holding) {
                                  _prefetcher.holding_insert(pf_line_addr, line_addr, raw_ip);
                                  *group_added_to_holding = true;
                                }
                              }});
      }
    } else if (cache_hit && useful_prefetch) {
      _prefetcher.rr_insert_and_train_acc(line_addr, raw_ip);
    } else if (!*group_added_to_holding) {
      _prefetcher.holding_insert(line_addr, line_addr, raw_ip);
      *group_added_to_holding = true;
    }

    _prefetcher.offsetLearning(line_addr, cache_hit);
    return {metadata_in, std::move(candidates)};
  }

  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                                     uint32_t metadata_in) final
  {
    const uint32_t metadata = _prefetcher.prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
    return {metadata, {}};
  }

private:
  caerus _prefetcher;
  CACHE* _cache;
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_caerus_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<CaerusPumpkinWrapper>(cache);
}
