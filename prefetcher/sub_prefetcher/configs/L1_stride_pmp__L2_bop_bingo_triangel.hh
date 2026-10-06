#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L1_STRIDE_PMP__L2_BOP_BINGO_TRIANGEL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L1_STRIDE_PMP__L2_BOP_BINGO_TRIANGEL_H

#include <array>

#include "../../bingo/pumpkin_wrapper.h"
#include "../../bop/pumpkin_wrapper.h"
#include "../../ip_stride/pumpkin_wrapper.h"
#include "../../pmp/pumpkin_wrapper.h"
#include "../../triangel/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

#define PUMPKIN_HAS_L1D_SUB_PREFETCHERS

namespace pumpkin_wrappers
{

inline constexpr auto L1D_SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"ip_stride", make_ip_stride_pumpkin_wrapper},
    SubPrefetcherConfig{"pmp", make_pmp_pumpkin_wrapper},
};

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bop", make_bop_pumpkin_wrapper},
    SubPrefetcherConfig{"bingo", make_bingo_pumpkin_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
