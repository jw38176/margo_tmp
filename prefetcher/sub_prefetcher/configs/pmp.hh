#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_PMP_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_PMP_H

#include <array>

#include "../../pmp/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"pmp", make_pmp_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
