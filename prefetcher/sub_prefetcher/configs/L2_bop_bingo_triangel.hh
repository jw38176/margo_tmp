#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L2_BOP_BINGO_TRIANGEL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L2_BOP_BINGO_TRIANGEL_H

#include <array>

#include "../../bop/pumpkin_wrapper.h"
#include "../../bingo/pumpkin_wrapper.h"
#include "../../triangel/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bop", make_bop_pumpkin_wrapper},
    SubPrefetcherConfig{"bingo", make_bingo_pumpkin_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
