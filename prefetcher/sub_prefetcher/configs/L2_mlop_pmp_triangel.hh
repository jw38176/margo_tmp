#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L2_MLOP_PMP_TRIANGEL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L2_MLOP_PMP_TRIANGEL_H

#include <array>

#include "../../mlop/pumpkin_wrapper.h"
#include "../../pmp/pumpkin_wrapper.h"
#include "../../triangel/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"mlop", make_mlop_pumpkin_wrapper},
    SubPrefetcherConfig{"pmp", make_pmp_pumpkin_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
