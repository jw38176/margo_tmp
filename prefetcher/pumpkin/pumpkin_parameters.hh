#ifndef PUMPKIN_PARAMETERS_H
#define PUMPKIN_PARAMETERS_H

#include <cstddef>
#include <cstdint>

namespace pumpkin_params
{
// Sampler size (L2-sized)
inline constexpr std::size_t SAMPLER_SETS = 64;
inline constexpr std::size_t SAMPLER_WAYS = 8;

// Criticality sampler size.
inline constexpr std::size_t CRIT_SAMPLER_SETS = 64;
inline constexpr std::size_t CRIT_SAMPLER_WAYS = 8;

// Filter Table Parameters
inline constexpr std::size_t FILTER_TABLE_SIZE = 512;
// Saturating Counter Bits
inline constexpr std::size_t MARGINAL_UTILITY_BITS = 6;
inline constexpr std::size_t ABSOLUTE_ACCURACY_BITS = 6;
inline constexpr std::size_t CRITICALITY_BITS = 6;

// Controller thresholds
inline constexpr uint64_t MARGINAL_UTILITY_THRESHOLD = 32;
inline constexpr uint64_t CRITICALITY_HI_THRESHOLD = 30;
inline constexpr uint64_t CRITICALITY_LO_THRESHOLD = 15;
inline constexpr uint64_t LLC_PENALTY = 1;
inline constexpr uint64_t DRAM_PENALTY = 5;
inline constexpr uint64_t LLC_REWARD = 1;
inline constexpr uint64_t DRAM_REWARD = 2;

// Coverage Filter Parameters
inline constexpr std::size_t COVERAGE_FILTER_SIZE = 256;
inline constexpr std::size_t COVERAGE_UTILITY_BITS = 6;

// Triangel tracking gate thresholds.
// On a covered access, Triangel is trained when Crit(Y) * (CU_SAT - CU(Y))
// exceeds TRIANGEL_TRACK_THRESHOLD, where both factors range over
// [0, (1 << *_BITS) - 1]. On a miss, only Crit(Y) is used.
inline constexpr uint64_t TRIANGEL_TRACK_THRESHOLD = 400;
inline constexpr uint64_t TRIANGEL_MISS_CRIT_THRESHOLD = CRITICALITY_LO_THRESHOLD;

// Probability (1-in-N) that a filtered-out prefetch is still issued, to maintain sampler-cache calibration.
inline constexpr uint64_t NEGATIVE_LEAK_INV_PROB = 100;

} // namespace pumpkin_params

#endif
