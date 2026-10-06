#ifndef PREFETCHER_SUB_PREFETCHER_CONFIG_H
#define PREFETCHER_SUB_PREFETCHER_CONFIG_H

// Active sub-prefetcher set
// #include "configs/L1.hh"
// #include "configs/L2.hh"
// #include "configs/next_line.hh"
// #include "configs/all.hh"
// #include "configs/berti_mlop.hh"
// #include "configs/bingo.hh"
// #include "configs/2pref.hh"
// #include "configs/3pref.hh"
// #include "configs/4pref.hh"
// #include "configs/5pref.hh"
// #include "configs/6pref.hh"

// *PLACE SCRIPT VARIABLE HERE*
#include "configs/L2.hh"
// L2.hh is the default config.

#include <cstddef>
#include <string>

namespace pumpkin_wrappers
{

inline const char* sub_prefetcher_name(std::size_t prefetcher_id)
{
  return prefetcher_id < SUB_PREFETCHERS.size() ? SUB_PREFETCHERS[prefetcher_id].name : "unknown";
}

struct SubPrefetcherSet {
  const SubPrefetcherConfig* first;
  std::size_t count;

  const SubPrefetcherConfig* begin() const { return first; }
  const SubPrefetcherConfig* end() const { return first + count; }
};

// Configs that define PUMPKIN_HAS_L1D_SUB_PREFETCHERS give L1D its own set
// (L1D_SUB_PREFETCHERS); every other cache uses SUB_PREFETCHERS.
inline SubPrefetcherSet sub_prefetchers_for(const std::string& cache_name)
{
#ifdef PUMPKIN_HAS_L1D_SUB_PREFETCHERS
  if (cache_name.find("L1D") != std::string::npos)
    return {L1D_SUB_PREFETCHERS.data(), L1D_SUB_PREFETCHERS.size()};
#else
  (void)cache_name;
#endif
  return {SUB_PREFETCHERS.data(), SUB_PREFETCHERS.size()};
}

} // namespace pumpkin_wrappers

#endif
