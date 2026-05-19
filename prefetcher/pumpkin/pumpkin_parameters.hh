#ifndef PUMPKIN_PARAMETERS_H
#define PUMPKIN_PARAMETERS_H

#include <cstddef>
#include <cstdint>

namespace pumpkin_params
{
// Sampler size (L2-sized)
inline constexpr std::size_t SAMPLER_SETS = 64;
inline constexpr std::size_t SAMPLER_WAYS = 8;

// Controller's per-PC filter table. Power-of-two required by FilterTable.
inline constexpr std::size_t FILTER_TABLE_SIZE = 4096;

// Probability (1-in-N) that a filtered-out prefetch is still issued, to maintain sampler-cache calibration.
inline constexpr uint64_t NEGATIVE_LEAK_INV_PROB = 100;

} // namespace pumpkin_params

#endif
