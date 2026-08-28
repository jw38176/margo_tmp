#ifndef PUMPKIN_NOCU_SAT_COUNTER_H
#define PUMPKIN_NOCU_SAT_COUNTER_H

#include <cstdint>

#include "../_rand/rand.hh"


namespace pumpkin_nocu_detail
{

// Regular (deterministic) Saturating Counter
struct rsc
{
  uint64_t current_value;
  uint64_t saturation_value;

  rsc(uint64_t num_bits = 3)
      : // Initialize to half the saturation value
        current_value((1ULL << num_bits) / 2), saturation_value((1ULL << num_bits) - 1)
  {
  }

  void increment()
  {
    if (current_value < saturation_value)
      ++current_value;
  }

  void decrement()
  {
    if (current_value > 0)
      --current_value;
  }

  uint64_t get_value() const { return current_value; }
};

// Stochastic Saturating Counter
struct ssc
{
  uint64_t current_value;
  uint64_t saturation_value;

  ssc(uint64_t num_bits = 3)
      : // Initialize to half the saturation value
        current_value((1ULL << num_bits) / 2), saturation_value((1ULL << num_bits) - 1)
  {
  }

  void increment()
  {
    if (current_value >= saturation_value)
      // Already saturated
      return;

    // Increment with probability (Max - n) / Max
    if (rand_u64() % saturation_value < saturation_value - current_value)
      ++current_value;
  }

  void decrement()
  {
    if (current_value == 0)
      // Already at 0
      return;

    // Decrement with probability n / Max
    if (rand_u64() % saturation_value < current_value)
      --current_value;
  }

  void update_toward(uint64_t numerator, uint64_t denominator)
  {
    if (denominator == 0)
      return;

    // current/sat vs numerator/den  ->  current*den vs numerator*sat
    if (current_value * denominator > numerator * saturation_value)
      decrement(); // current too high -> decrement with probability current/sat
    else
      increment(); // current too low  -> increment with probability (sat-current)/sat
  }

  uint64_t get_value() const { return current_value; }
};

// Probabilistic Saturating Counter
struct psc
{
  uint64_t current_value;
  uint64_t saturation_value;

  psc(uint64_t num_bits = 3)
      : // Initialize to half the saturation value
        current_value((1ULL << num_bits) / 2), saturation_value((1ULL << num_bits) - 1)
  {
  }

  // Increment with probability numerator / denominator
  void increment(uint64_t numerator, uint64_t denominator)
  {
    if (current_value >= saturation_value)
      // Already saturated
      return;

    if (denominator == 0)
      return;

    if (rand_u64() % denominator < numerator)
      current_value++;
  }

  // Decrement with probability numerator / denominator
  void decrement(uint64_t numerator, uint64_t denominator)
  {
    if (current_value == 0)
      // Already at 0
      return;

    if (denominator == 0)
      return;

    if (rand_u64() % denominator < numerator)
      --current_value;
  }

  uint64_t get_value() const { return current_value; }
};

} // namespace pumpkin_nocu_detail

#endif