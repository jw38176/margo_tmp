#ifndef ALECTO_PARAMETERS_H
#define ALECTO_PARAMETERS_H

#include <cstddef>
#include <cstdint>

namespace alecto_params
{
// ─── Table geometry (mirrors the original Alecto hardware tables) ────────────────

// Allocation table: per-PC, per-sub-prefetcher control "state" that drives degree.
inline constexpr std::size_t ALLOCATION_TABLE_SIZE = 64;

// Sample table: per-PC issued/confirmed accounting over a learning phase.
inline constexpr std::size_t SAMPLE_TABLE_SIZE = 64;

// Sandbox table: recently-issued prefetch blocks. Doubles as a dedup filter and a
// usefulness tracker (a later demand hit confirms the prefetch).
inline constexpr std::size_t SANDBOX_TABLE_SIZE = 512;

// Index/tag split for the PC-indexed tables (allocation + sample).
//   index = xor_fold(pc, PC_INDEX_BITS) % {ALLOCATION,SAMPLE}_TABLE_SIZE
//   tag   = (pc >> PC_INDEX_BITS) & ((1 << PC_TAG_BITS) - 1)
inline constexpr int PC_INDEX_BITS = 6; // log2(64)
inline constexpr int PC_TAG_BITS = 9;

// Index/tag split for the block-indexed sandbox table.
inline constexpr int SANDBOX_INDEX_BITS = 9; // log2(512)
inline constexpr int SANDBOX_TAG_BITS = 6;

// ─── Phase / state-machine constants ─────────────────────────────────────────────

// A learning phase for a tracked PC ends after this many demands; accuracy is then
// folded into the allocation table and the issued/confirmed counters are cleared.
inline constexpr uint16_t PERIOD = 100;

// Consecutive "dead" demands (a tracked PC that issued no prefetch since its previous
// demand) after which the PC's prefetching is reset.
inline constexpr uint16_t DEAD_THRESH = 150;

// Default degree applied to every sub-prefetcher when a PC is unprofiled, and the
// degree floor for an active sub-prefetcher. Degree = BASE_DEGREE + state when state>0.
inline constexpr int BASE_DEGREE = 3;

// Upper bound for a positive allocation state (degree caps at BASE_DEGREE + MAX_STATE).
inline constexpr int MAX_STATE = 6;

// Value loaded into a state when a sub-prefetcher is found to be near-useless; it then
// counts back up toward 0 one step per phase before it may be retried.
inline constexpr int BLOCK_STATE = -9;

// Accuracy thresholds (confirmed / issued) that drive the state machine.
inline constexpr double ACC_HIGH = 0.75; // promote / keep active
inline constexpr double ACC_LOW = 0.05;  // demote hard (block)

} // namespace alecto_params

#endif
