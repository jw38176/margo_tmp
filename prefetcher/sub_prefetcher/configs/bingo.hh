#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_IP_STRIDE_BINGO_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_IP_STRIDE_BINGO_H

#include <array>

#include "../../bingo/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"bingo", make_bingo_margo_wrapper},
};

} // namespace margo_wrappers

#endif
