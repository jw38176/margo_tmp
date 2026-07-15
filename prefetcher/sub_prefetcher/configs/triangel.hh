#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_ALL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_ALL_H

#include <array>

#include "../../triangel/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"triangel", make_triangel_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
