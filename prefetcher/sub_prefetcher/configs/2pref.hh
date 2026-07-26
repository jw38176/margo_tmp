#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_2PREF_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_2PREF_H

#include <array>

#include "../../ip_stride/pumpkin_wrapper.h"
#include "../../next_line/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"ip_stride", make_ip_stride_pumpkin_wrapper},
    SubPrefetcherConfig{"next_line", make_next_line_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
