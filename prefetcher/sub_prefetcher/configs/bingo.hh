#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_IP_STRIDE_BINGO_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_IP_STRIDE_BINGO_H

#include <array>

#include "../../bingo/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bingo", make_bingo_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
