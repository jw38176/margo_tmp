#include "margo_wrapper.h"

#include <memory>

#include "address.h"

namespace
{

class NextLineMargoWrapper final : public margo_wrappers::WrappedPrefetcher
{
public:
  explicit NextLineMargoWrapper(CACHE* cache) { (void)cache; }

  margo_wrappers::WrappedPrefetchResult cache_operate(champsim::address addr, champsim::address ip, uint8_t, bool, access_type, uint32_t metadata_in) final
  {
    champsim::block_number pf_addr{addr};
    champsim::address next_addr{pf_addr + 1};
    return {metadata_in, {{next_addr, ip, true, metadata_in}}};
  }

  margo_wrappers::WrappedPrefetchResult cache_fill(champsim::address, long, long, uint8_t, champsim::address, uint32_t metadata_in) final
  {
    return {metadata_in, {}};
  }
};

} // namespace

std::unique_ptr<margo_wrappers::WrappedPrefetcher> make_next_line_margo_wrapper(CACHE* cache)
{
  return std::make_unique<NextLineMargoWrapper>(cache);
}
