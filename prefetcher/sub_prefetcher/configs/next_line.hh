#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_NEXT_LINE_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_NEXT_LINE_H

#include <array>

#include "../../next_line/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"next_line", make_next_line_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
