#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_L2_NO_IP_STRIDE_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_L2_NO_IP_STRIDE_H

#include <array>

#include "../../berti/margo_wrapper.h"
#include "../../bingo/margo_wrapper.h"
#include "../../mlop/margo_wrapper.h"
#include "../../next_line/margo_wrapper.h"
#include "../../pmp/margo_wrapper.h"
#include "../../pythia/margo_wrapper.h"
#include "../../triangel/margo_wrapper.h"
#include "../sub_prefetcher_common.hh"

namespace margo_wrappers
{

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"berti", make_berti_margo_wrapper},
    SubPrefetcherConfig{"bingo", make_bingo_margo_wrapper},
    SubPrefetcherConfig{"pythia", make_pythia_margo_wrapper},
    SubPrefetcherConfig{"next_line", make_next_line_margo_wrapper},
    SubPrefetcherConfig{"mlop", make_mlop_margo_wrapper},
    SubPrefetcherConfig{"pmp", make_pmp_margo_wrapper},
    SubPrefetcherConfig{"triangel", make_triangel_margo_wrapper},
};

} // namespace margo_wrappers

#endif
