#ifndef PUMPKIN_NOCU_MASKING_H
#define PUMPKIN_NOCU_MASKING_H

#include <cstdint>

#include "address.h"
#include "pumpkin_nocu_parameters.hh"

// Storage-efficiency helpers shared by the samplers and the controller. Raw
// cycle counts and line addresses are truncated to a fixed width before being
// stored, so all arithmetic on the stored values must be wrap-safe (modular).
namespace pumpkin_nocu_masking
{

// Truncate a raw cycle to TIMESTAMP_BITS. A masked result of 0 is remapped to 1
// so that 0 stays reserved as the "not set" sentinel used throughout the
// samplers (a genuine timestamp never masks to 0).
inline uint64_t mask_timestamp(uint64_t cycle)
{
  const uint64_t m = cycle & pumpkin_nocu_params::TIMESTAMP_MASK;
  return m == 0 ? 1 : m;
}

// Modular difference between two masked timestamps. Correct (equals the true
// gap) whenever the real interval is smaller than 2^TIMESTAMP_BITS.
inline uint64_t timestamp_delta(uint64_t later, uint64_t earlier)
{
  return (later - earlier) & pumpkin_nocu_params::TIMESTAMP_MASK;
}

// True when `earlier` plausibly precedes `later`: the modular gap is non-zero
// and within the plausibility window (i.e. not a wrap artifact of a very old or
// out-of-order timestamp).
inline bool timestamp_before(uint64_t earlier, uint64_t later)
{
  const uint64_t gap = timestamp_delta(later, earlier);
  return gap != 0 && gap <= pumpkin_nocu_params::TIMESTAMP_HALF;
}

// Hash a cache-line address down to a compact LINE_TAG_BITS tag. The murmur-3
// finalizer mixes the block number so the low bits (used to index the sampler
// tables) are well distributed; the result is masked to the tag width.
inline uint64_t make_line_tag(champsim::address line_addr)
{
  uint64_t x = champsim::block_number{line_addr}.to<uint64_t>();
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x & pumpkin_nocu_params::LINE_TAG_MASK;
}

} // namespace pumpkin_nocu_masking

#endif
