#ifndef NEXT_LINE_MARGO_WRAPPER_H
#define NEXT_LINE_MARGO_WRAPPER_H

#include <memory>

#include "../sub_prefetcher/wrapped_prefetcher.hh"

class CACHE;

std::unique_ptr<margo_wrappers::WrappedPrefetcher> make_next_line_margo_wrapper(CACHE* cache);

#endif
