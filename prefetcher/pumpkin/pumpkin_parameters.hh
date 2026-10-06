#ifndef PUMPKIN_PARAMETERS_H
#define PUMPKIN_PARAMETERS_H

#include <cstddef>
#include <cstdint>

namespace pumpkin_params
{
// Sampler size (L2-sized)
inline constexpr std::size_t SAMPLER_SETS = 8;
inline constexpr std::size_t SAMPLER_WAYS = 4;

// Criticality sampler size.
inline constexpr std::size_t CRIT_SAMPLER_SETS = 64;
inline constexpr std::size_t CRIT_SAMPLER_WAYS = 8;

// Filter Table Parameters
inline constexpr std::size_t FILTER_TABLE_SIZE = 128;
// Saturating Counter Bits
inline constexpr std::size_t MARGINAL_UTILITY_BITS = 4;
inline constexpr std::size_t ABSOLUTE_ACCURACY_BITS = 4;
inline constexpr std::size_t CRITICALITY_BITS = 4;

// Controller thresholds
inline constexpr uint64_t MARGINAL_UTILITY_THRESHOLD = 16;
inline constexpr uint64_t LLC_PENALTY = 1;
inline constexpr uint64_t DRAM_PENALTY = 5;
inline constexpr uint64_t LLC_REWARD = 3;
inline constexpr uint64_t DRAM_REWARD = 5;

// Coverage Filter Parameters
inline constexpr std::size_t COVERAGE_FILTER_SIZE = 256;
inline constexpr std::size_t COVERAGE_UTILITY_BITS = 6;

// Temporary criticality tunning knobs
inline constexpr bool CU_CRIT_ON = false;
inline constexpr bool CU_ON = true;
inline constexpr float CU_CRIT_SCALE = 0.125; // from 0 to 1
inline constexpr float MU_CRIT_CONSTANT = 3;  // C

// Triangel tracking gate thresholds.
// On a covered access, Triangel is trained when Crit(Y) * (CU_SAT - CU(Y))
// exceeds TRIANGEL_TRACK_THRESHOLD, where both factors range over
// [0, (1 << *_BITS) - 1].
inline constexpr uint64_t TRIANGEL_TRACK_THRESHOLD = 800;
inline constexpr uint64_t TEST_THRESHOLD = 20;

// When true, filtered-out prefetches may still be issued at a 1-in-N rate so the
// sampler cache stays calibrated. Set to false to disable negative leakage entirely.
inline constexpr bool ENABLE_NEGATIVE_LEAK = true;
inline constexpr uint64_t NEGATIVE_LEAK_INV_PROB = 100;

// Masks
inline constexpr uint64_t TIMESTAMP_BITS = 14;
inline constexpr uint64_t TIMESTAMP_MASK = (1ULL << TIMESTAMP_BITS) - 1;
inline constexpr uint64_t TIMESTAMP_HALF = 1ULL << (TIMESTAMP_BITS - 1);
inline constexpr uint64_t LINE_TAG_BITS = 14;
inline constexpr uint64_t LINE_TAG_MASK = (1ULL << LINE_TAG_BITS) - 1;

} // namespace pumpkin_params

#endif
