#ifndef BOP_HT_PUMPKIN_WRAPPER_H
#define BOP_HT_PUMPKIN_WRAPPER_H

#include <memory>

#include "../sub_prefetcher/wrapped_prefetcher.hh"

class CACHE;

std::unique_ptr<pumpkin_wrappers::WrappedPrefetcher> make_bop_ht_pumpkin_wrapper(CACHE* cache);

#endif
