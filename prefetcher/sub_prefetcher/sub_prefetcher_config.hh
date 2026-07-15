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


namespace pumpkin_wrappers
{

inline const char* sub_prefetcher_name(std::size_t prefetcher_id)
{
  return prefetcher_id < SUB_PREFETCHERS.size() ? SUB_PREFETCHERS[prefetcher_id].name : "unknown";
}

} // namespace pumpkin_wrappers

#endif
