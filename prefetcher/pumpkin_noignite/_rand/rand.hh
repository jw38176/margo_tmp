#ifndef PUMPKIN_NOIGNITE_RAND_H
#define PUMPKIN_NOIGNITE_RAND_H

#include <cstdint>

namespace pumpkin_noignite_detail
{


inline uint64_t rand_u64()
{
  static uint64_t _rng_state{0xD1B54A32D192ED03ULL};
  uint64_t x = _rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  _rng_state = x;
  return x;
}


} // namespace pumpkin_noignite_detail

#endif