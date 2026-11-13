#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_MLOP_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_MLOP_H

#include <array>

#include "../../mlop/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"mlop", make_mlop_margo_wrapper},
};

} // namespace margo_wrappers

#endif
