#ifndef PREFETCHER_BOP_PARAMETERS_H
#define PREFETCHER_BOP_PARAMETERS_H

#include <cstddef>

// Names are BOP_-prefixed because caerus_parameters.h #defines SCORE_MAX, RR_SIZE, etc. and
// ChampSim includes the headers of every configured module into the same translation unit.
namespace bop_space
{
/*****************************************************************************
 *                              PARAMS                                       *
 *****************************************************************************/
// BOP
inline constexpr unsigned int BOP_SCORE_MAX = 31;         // Max. score to update the best offset
inline constexpr unsigned int BOP_ROUND_MAX = 100;        // Max. round to update the best offset
inline constexpr unsigned int BOP_BAD_SCORE = 1;          // Score at which the HWP is disabled
inline constexpr unsigned int BOP_RR_SIZE = 256;          // Number of entries in RR Table
inline constexpr unsigned int BOP_TAG_BITS = 12;          // Bits used to store the tag
inline constexpr unsigned int BOP_OFFSET_LIST_SIZE = 104; // Number of entries in the offsets list

// Toggle to include negative offsets in the offset list
// If enabled, half of the offset list will contain negative offsets.
inline constexpr bool BOP_NEGATIVE_OFFSETS_ENABLE = true;

// Pumpkin wrapper only: number of issued-but-not-yet-filled BOP prefetches remembered so that
// BOP can recognise its own prefetch fills. Must cover the in-flight prefetches (PQ + MSHR).
inline constexpr std::size_t BOP_ISSUED_TRACKER_SIZE = 64;

#ifdef BOP_DBUG
inline constexpr bool BOP_DEBUG = true;
#else
inline constexpr bool BOP_DEBUG = false;
#endif
} // namespace bop_space

#endif
