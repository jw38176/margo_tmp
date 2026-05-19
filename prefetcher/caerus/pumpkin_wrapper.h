#ifndef CAERUS_PUMPKIN_WRAPPER_H
#define CAERUS_PUMPKIN_WRAPPER_H

#include <memory>

#include "../pumpkin/wrapped_prefetcher.hh"

class CACHE;

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_caerus_pumpkin_wrapper(CACHE* cache);

#endif
