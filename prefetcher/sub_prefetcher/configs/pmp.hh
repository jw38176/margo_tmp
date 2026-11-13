#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_PMP_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_PMP_H

#include <array>

#include "../../pmp/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"pmp", make_pmp_margo_wrapper},
};

} // namespace margo_wrappers

#endif
