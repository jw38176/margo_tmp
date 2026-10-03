#include "pumpkin_wrapper.h"

#include <algorithm>
#include <deque>
#include <memory>

#include "bop.h"

namespace
{

class BopPumpkinWrapper final : public pumpkin_wrappers::WrappedPrefetcher
{
public:
  explicit BopPumpkinWrapper(CACHE* cache) : _prefetcher(cache) {}

  void initialize() final { _prefetcher.prefetcher_initialize(); }

  pumpkin_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                        access_type type, uint32_t metadata_in) final
  {
    auto pf_addr = _prefetcher.operate(addr, cache_hit, useful_prefetch, type);
    if (!pf_addr.has_value()) {
      return {metadata_in, {}};
    }

    const champsim::block_number pf_block{*pf_addr};
    return {metadata_in, {{*pf_addr, ip, true, 0x1, [this, pf_block](bool issued, bool /*filtered_out*/) {
                             if (issued) {
                               track_issued(pf_block);
                             }
                           }}}};
  }

  // Other sub-prefetchers share the 0x1 metadata and metadata is chained through them, so a fill
  // counts as a BOP prefetch fill only if BOP itself issued that block.
  pumpkin_wrappers::WrappedPrefetchResult cache_fill(champsim::address addr, long, long, uint8_t prefetch, champsim::address, uint32_t metadata_in) final
  {
    const bool bop_prefetch = untrack_issued(champsim::block_number{addr});
    _prefetcher.insertFill(addr.to<uint64_t>(), prefetch, bop_prefetch);
    return {metadata_in, {}};
  }

private:
  void track_issued(champsim::block_number block)
  {
    if (std::find(_issued.begin(), _issued.end(), block) != _issued.end()) {
      return;
    }
    _issued.push_back(block);
    if (_issued.size() > bop_space::BOP_ISSUED_TRACKER_SIZE) {
      _issued.pop_front();
    }
  }

  bool untrack_issued(champsim::block_number block)
  {
    auto it = std::find(_issued.begin(), _issued.end(), block);
    if (it == _issued.end()) {
      return false;
    }
    _issued.erase(it);
    return true;
  }

  bop _prefetcher;
  std::deque<champsim::block_number> _issued;
};

} // namespace

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_bop_pumpkin_wrapper(CACHE* cache)
{
  return std::make_unique<BopPumpkinWrapper>(cache);
}
