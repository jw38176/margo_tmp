#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L1_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L1_H

#include <array>

#include "../../bingo/margo_wrapper.h"
#include "../../caerus/margo_wrapper.h"
#include "../../ip_stride/margo_wrapper.h"
#include "../../pythia/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"ip_stride", make_ip_stride_margo_wrapper},
    SubPrefetcherConfig{"bingo", make_bingo_margo_wrapper},
    SubPrefetcherConfig{"pythia", make_pythia_margo_wrapper},
    SubPrefetcherConfig{"caerus", make_caerus_margo_wrapper},
};

} // namespace margo_wrappers

#endif
