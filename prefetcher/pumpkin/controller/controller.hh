#ifndef PUMPKIN_CONTROLLER_H
#define PUMPKIN_CONTROLLER_H

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <vector>

#include "../_sat_counter/sat_counter.hh"
#include "../crit_sampler/crit_sampler.hh"
#include "../pf_sampler/pf_sampler.hh"
#include "../pumpkin_parameters.hh"
#include "address.h"

class CACHE;

struct utility_tracker_entry
{
  psc marginal_utility{pumpkin_params::MARGINAL_UTILITY_BITS};
  ssc absolute_accuracy{pumpkin_params::ABSOLUTE_ACCURACY_BITS};
  ssc criticality{pumpkin_params::CRITICALITY_BITS};
};

// Per-PC Coverage Utility (CU). Unlike utility_tracker_entry, CU is aggregated
// over all non-Triangel prefetchers, so there is a single counter per PC.
struct coverage_entry
{
  ssc coverage_utility{pumpkin_params::COVERAGE_UTILITY_BITS};
};

class FilterTable
{
public:
  explicit FilterTable(std::size_t size, std::size_t num_prefetchers);
  ~FilterTable();

  utility_tracker_entry& get(champsim::address pc, std::size_t prefetcher_id);
  const utility_tracker_entry& get(champsim::address pc, std::size_t prefetcher_id) const;

  // Prefetcher ids in descending absolute accuracy. Ties preserve lower id.
  std::vector<std::size_t> rank_prefetchers_by_absolute_accuracy(champsim::address pc) const;

  void print_stats(std::ostream& os, const std::function<const char*(std::size_t)>& prefetcher_name) const;

private:
  std::size_t _size;
  std::size_t _num_prefetchers;

  std::vector<std::vector<utility_tracker_entry>> _filter_table; // PC-indexed

  std::size_t getIndex(champsim::address pc) const;
};

// PC-indexed table holding the Coverage Utility counter for each PC. Structured
// like FilterTable, but with a single counter per entry (CU is not per-prefetcher).
class CoverageFilter
{
public:
  explicit CoverageFilter(std::size_t size);
  ~CoverageFilter();

  coverage_entry& get(champsim::address pc);
  const coverage_entry& get(champsim::address pc) const;

  void print_stats(std::ostream& os) const;

private:
  std::size_t _size;

  std::vector<coverage_entry> _coverage_filter; // PC-indexed

  std::size_t getIndex(champsim::address pc) const;
};

class Controller
{

public:
  Controller(std::size_t num_prefetchers, std::size_t filter_table_size,
             const std::function<const char*(std::size_t)>& prefetcher_name = {});
  ~Controller();

  void process_eviction_event(const PfSampler::EvictionEvent& event, uint64_t current_cycle);

  // Provide the last-level cache reference so training can tell whether an
  // evicted line still resides deeper in the hierarchy. Set once at startup.
  void set_llc(CACHE* llc) { _llc = llc; }

  // True if the line containing addr is currently resident in the LLC. Always
  // false when no LLC reference has been set.
  bool in_llc(champsim::address addr) const;

  // Updates the per-PC criticality counter from a completed crit-sampler entry.
  // A load is critical when it reached the ROB head sooner than its counterfactual
  // fill (demand_issue_time + natural_latency), i.e. a (pre)fetch pulled the
  // ROB-head event in.
  void process_criticality_entry(const CritSampler::crit_sampler_entry& entry);
  void process_criticality_event(const CritSampler::EvictionEvent& event);

  bool should_prefetch(champsim::address access_pc, std::size_t prefetcher_id);

  // Gate for whether Triangel should be told about (trained on) an access at
  // this PC. On a covered access the product Crit(Y) * (1 - CU(Y)) is compared
  // against a threshold; on a miss only Crit(Y) is used.
  bool should_track_with_triangel(champsim::address pc, bool is_miss) const;

  // Prefetcher ids in descending absolute accuracy. Ties preserve lower id.
  std::vector<std::size_t> rank_prefetchers_by_absolute_accuracy(champsim::address pc) const;

  // For stats reporting
  uint64_t critical_loads_recorded() const { return _critical_loads_recorded; }
  uint64_t non_critical_loads_recorded() const { return _non_critical_loads_recorded; }

  // Negative-path (inaccurate prefetch) origin breakdown. Classified exactly as
  // the marginal-utility penalty: for a filled line (fill_latency != 0) the
  // origin is the ground-truth from_dram bit; for a non-filled line
  // (fill_latency == 0) it is approximated by LLC residency (resident -> LLC,
  // otherwise assumed DRAM).
  uint64_t inaccurate_dram_fill_zero() const { return _inaccurate_dram_fill_zero; }
  uint64_t inaccurate_dram_fill_nonzero() const { return _inaccurate_dram_fill_nonzero; }
  uint64_t inaccurate_llc_fill_zero() const { return _inaccurate_llc_fill_zero; }
  uint64_t inaccurate_llc_fill_nonzero() const { return _inaccurate_llc_fill_nonzero; }

  void print_filter_table_stats(std::ostream& os, const std::function<const char*(std::size_t)>& prefetcher_name) const;
  void print_coverage_filter_stats(std::ostream& os) const;

private:
  FilterTable _filter_table;
  CoverageFilter _coverage_filter;

  // Last-level cache, used by in_llc(). Null until set_llc() is called.
  CACHE* _llc{nullptr};

  // Prefetcher id of Triangel, detected by name at construction. SIZE_MAX if
  // Triangel is not part of the active sub-prefetcher set.
  std::size_t _triangel_id{SIZE_MAX};

  // Stats
  std::size_t _num_prefetchers{0};
  uint64_t _filter_stats_events{0};
  uint64_t _critical_loads_recorded{0};
  uint64_t _non_critical_loads_recorded{0};

  // Negative-path inaccurate-prefetch origin counts (see accessors above).
  uint64_t _inaccurate_dram_fill_zero{0};
  uint64_t _inaccurate_dram_fill_nonzero{0};
  uint64_t _inaccurate_llc_fill_zero{0};
  uint64_t _inaccurate_llc_fill_nonzero{0};
};

#endif
