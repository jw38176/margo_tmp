#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L2_BOP_PMP_TRIANGEL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L2_BOP_PMP_TRIANGEL_H

#include <array>

#include "../../bop/margo_wrapper.h"
#include "../../pmp/margo_wrapper.h"
#include "../../triangel/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bop", make_bop_margo_wrapper},
    SubPrefetcherConfig{"pmp", make_pmp_margo_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_margo_wrapper},
};

} // namespace margo_wrappers

#endif
