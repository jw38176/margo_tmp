#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L1_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L1_H

#include <array>

#include "../../bingo/pumpkin_wrapper.h"
#include "../../caerus/pumpkin_wrapper.h"
#include "../../ip_stride/pumpkin_wrapper.h"
#include "../../pythia/pumpkin_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace pumpkin_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"ip_stride", make_ip_stride_pumpkin_wrapper},
    SubPrefetcherConfig{"bingo", make_bingo_pumpkin_wrapper},
    SubPrefetcherConfig{"pythia", make_pythia_pumpkin_wrapper},
    SubPrefetcherConfig{"caerus", make_caerus_pumpkin_wrapper},
};

} // namespace pumpkin_wrappers

#endif
