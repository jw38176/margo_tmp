#ifndef PREFETCHER_PUMPKIN_SUB_PREFETCHER_CONFIG_H
#define PREFETCHER_PUMPKIN_SUB_PREFETCHER_CONFIG_H

#include <array>
#include <cstddef>
#include <memory>

#include "../berti/pumpkin_wrapper.h"
#include "../caerus/pumpkin_wrapper.h"
#include "../gaze/pumpkin_wrapper.h"
#include "../ip_stride/pumpkin_wrapper.h"
#include "../pythia/pumpkin_wrapper.h"
#include "../spp_ppf/pumpkin_wrapper.h"
#include "wrapped_prefetcher.hh"

class CACHE;

namespace pumpkin_wrappers
{

using WrappedPrefetcherFactory = std::unique_ptr<WrappedPrefetcher> (*)(CACHE* cache);

struct SubPrefetcherConfig
{
  const char* name;
  WrappedPrefetcherFactory make;
};

inline constexpr auto SUB_PREFETCHERS = std::array{
    SubPrefetcherConfig{"ip_stride", make_ip_stride_pumpkin_wrapper},
    SubPrefetcherConfig{"caerus", make_caerus_pumpkin_wrapper},
    // SubPrefetcherConfig{"berti", make_berti_pumpkin_wrapper},
    SubPrefetcherConfig{"gaze", make_gaze_pumpkin_wrapper},
    // SubPrefetcherConfig{"spp_ppf", make_spp_ppf_pumpkin_wrapper},
    SubPrefetcherConfig{"pythia", make_pythia_pumpkin_wrapper},
};

inline const char* sub_prefetcher_name(std::size_t prefetcher_id)
{
  return prefetcher_id < SUB_PREFETCHERS.size() ? SUB_PREFETCHERS[prefetcher_id].name : "unknown";
}

} // namespace pumpkin_wrappers

#endif
