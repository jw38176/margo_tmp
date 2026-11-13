#ifndef MARGO_PARAMETERS_H
#define MARGO_PARAMETERS_H

#include <cstddef>
#include <cstdint>

namespace margo_params
{
// Sampler size (L2-sized)
inline constexpr std::size_t SAMPLER_SETS = 64;
inline constexpr std::size_t SAMPLER_WAYS = 8;

// Filter Table Parameters
inline constexpr std::size_t FILTER_TABLE_SIZE = 128;
// Saturating Counter Bits
inline constexpr std::size_t MARGINAL_UTILITY_BITS = 6;
inline constexpr std::size_t ABSOLUTE_ACCURACY_BITS = 6;    

// Controller thresholds
inline constexpr uint64_t MARGINAL_UTILITY_THRESHOLD = 32;
inline constexpr uint64_t LLC_PENALTY = 1;
inline constexpr uint64_t DRAM_PENALTY = 5;
inline constexpr uint64_t LLC_REWARD = 3;
inline constexpr uint64_t DRAM_REWARD = 5;

// Coverage Filter Parameters
inline constexpr std::size_t COVERAGE_FILTER_SIZE = 256;
inline constexpr std::size_t COVERAGE_UTILITY_BITS = 6;

// Triangel tracking gate threshold.
// Triangel is trained when the inverse coverage utility (CU_SAT - CU(Y))
// exceeds TEST_THRESHOLD.
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

} // namespace margo_params

#endif
