#ifndef PPF_PARAMETERS_H
#define PPF_PARAMETERS_H

#include <cstdint>

namespace ppf_params
{

// ─── Perceptron geometry ─────────────────────────────────────────────────────────

inline constexpr int PERC_ENTRIES = 4096;
inline constexpr int PERC_FEATURES = 8;
inline constexpr int PERC_COUNTER_MAX = 15; // -16 to +15: 5-bit counter

// Prediction thresholds: 
//   >= PERC_THRESHOLD_HI  → fill this cache level
//   >= PERC_THRESHOLD_LO  → fill lower level only
//   <  PERC_THRESHOLD_LO  → reject
inline constexpr int PERC_THRESHOLD_HI = 10;
inline constexpr int PERC_THRESHOLD_LO = 6;

// Training hysteresis: only reinforce a correct prediction whose sum lies in
// (NEG_UPDT_THRESHOLD, POS_UPDT_THRESHOLD). Saturated sums are left alone.
inline constexpr int POS_UPDT_THRESHOLD = 90;
inline constexpr int NEG_UPDT_THRESHOLD = -80;

// ─── Prefetch filter (tracks issued prefetches for usefulness feedback) ──────────

inline constexpr int QUOTIENT_BIT = 10;
inline constexpr int REMAINDER_BIT = 4;
inline constexpr int FILTER_SET = 1 << QUOTIENT_BIT; // 1024

// ─── Reject filter (tracks perceptron rejections for negative training) ──────────

inline constexpr int QUOTIENT_BIT_REJ = 10;
inline constexpr int REMAINDER_BIT_REJ = 4;
inline constexpr int FILTER_SET_REJ = 1 << QUOTIENT_BIT_REJ; // 1024

// ─── Global accuracy counter ─────────────────────────────────────────────────────

inline constexpr uint64_t GLOBAL_COUNTER_MAX = 1023;

} // namespace ppf_params

#endif
