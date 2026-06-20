#ifndef IPOP_PARAMETERS_H
#define IPOP_PARAMETERS_H

#include <cstddef>
#include <cstdint>

namespace ipop_params
{
// Usefulness-tracking table: maps a prefetched line -> issuing sub-prefetcher. Power-of-two size.
inline constexpr std::size_t PF_ADDR_TABLE_SIZE = 512;
inline constexpr unsigned PF_ADDR_TABLE_TAG_BITS = 16;

// Pollution-tracking table: maps a line evicted by a prefetch -> issuing sub-prefetcher. Power-of-two size.
inline constexpr std::size_t POLL_ADDR_TABLE_SIZE = 512;
inline constexpr unsigned POLL_ADDR_TABLE_TAG_BITS = 16;

// log2(PF_ADDR_TABLE_SIZE) / log2(POLL_ADDR_TABLE_SIZE); kept here so index/tag splitting stays in sync.
inline constexpr unsigned PF_ADDR_TABLE_INDEX_BITS = 9;
inline constexpr unsigned POLL_ADDR_TABLE_INDEX_BITS = 9;

// A control phase ends after this many demand accesses; degrees are re-evaluated at every phase boundary.
inline constexpr uint64_t PHASE_LENGTH = 4096;

// Number of phases a net-harmful prefetcher is kept blocked before it is allowed to re-enter.
inline constexpr int BLOCK_PHASE = 3;

// Per-prefetcher degree levels (issue aggressiveness).
inline constexpr int LOW_DEGREE = 2;
inline constexpr int MID_DEGREE = 6;  // initial degree for every sub-prefetcher
inline constexpr int HIGH_DEGREE = 10;
inline constexpr int DEGREE_STEP = 2;

// Congestion proxy thresholds on the average MSHR occupancy ratio. The original IPOP/UDC design uses
// measured DRAM access latency as its congestion signal; the modular ChampSim cache model does not expose
// DRAM latency to a prefetcher module, so MSHR pressure is used as a faithful, locally-available stand-in.
inline constexpr double CONGESTION_HIGH = 0.75; // above this, throttle the least-useful prefetcher
inline constexpr double CONGESTION_LOW = 0.40;  // below this, promote the most-useful / reactivate idle prefetchers

} // namespace ipop_params

#endif
