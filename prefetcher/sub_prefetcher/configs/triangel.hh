#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_ALL_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_ALL_H

#include <array>

#include "../../triangel/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"triangel", make_triangel_margo_wrapper},
};

} // namespace margo_wrappers

#endif
