#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L2_BOP_HT_PMP_TRIANGEL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L2_BOP_HT_PMP_TRIANGEL_H

#include <array>

#include "../../bop_ht/margo_wrapper.h"
#include "../../pmp/margo_wrapper.h"
#include "../../triangel/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bop_ht", make_bop_ht_margo_wrapper},
    SubPrefetcherConfig{"pmp", make_pmp_margo_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_margo_wrapper},
};

} // namespace margo_wrappers

#endif
