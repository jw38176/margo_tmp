#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_MLOP_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_MLOP_H

#include <array>

#include "../../mlop/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"mlop", make_mlop_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
