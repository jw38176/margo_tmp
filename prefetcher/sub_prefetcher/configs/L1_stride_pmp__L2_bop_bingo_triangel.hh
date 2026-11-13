#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L1_STRIDE_PMP__L2_BOP_BINGO_TRIANGEL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L1_STRIDE_PMP__L2_BOP_BINGO_TRIANGEL_H

#include <array>

#include "../../bingo/margo_wrapper.h"
#include "../../bop/margo_wrapper.h"
#include "../../ip_stride/margo_wrapper.h"
#include "../../pmp/margo_wrapper.h"
#include "../../triangel/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

#define MARGO_HAS_L1D_SUB_PREFETCHERS

namespace margo_wrappers
{

inline constexpr auto L1D_SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"ip_stride", make_ip_stride_margo_wrapper},
    SubPrefetcherConfig{"pmp", make_pmp_margo_wrapper},
};

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bop", make_bop_margo_wrapper},
    SubPrefetcherConfig{"bingo", make_bingo_margo_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_margo_wrapper},
};

} // namespace margo_wrappers

#endif
