#ifndef PREFETCHER_SUB_PREFETCHER_COMMON_H
#define PREFETCHER_SUB_PREFETCHER_COMMON_H

#include <cstddef>
#include <memory>

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

} // namespace pumpkin_wrappers

#endif
