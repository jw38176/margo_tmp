#ifndef PUMPKIN_CONTROLLER_H
#define PUMPKIN_CONTROLLER_H

#include <vector>

#include "../pf_sampler/pf_sampler.hh"
#include "address.h"

struct utility_tracker_entry
{
  double marginal_utility = 0.0;
  uint64_t inacc_pf_count = 0;
  uint64_t acc_pf_count = 0;
};

class FilterTable
{
public:
  explicit FilterTable(std::size_t size, std::size_t num_prefetchers);
  ~FilterTable();

  utility_tracker_entry& get(champsim::address pc, std::size_t prefetcher_id);
  void decay_stats(double decay_factor, double reset_threshold);

private:
  std::size_t _size;
  std::size_t _num_prefetchers;

  std::size_t getIndex(champsim::address pc) const;

  std::vector<std::vector<utility_tracker_entry>> _table; // PC indexed table of utility values
};

class Controller
{

public:
  explicit Controller(std::size_t num_prefetchers, std::size_t filter_table_size);
  ~Controller();

  // Types of sample-entry events:
  //  - Access: demand accessed a prefetched line (positive training)
  //  - Evict: a prefetched line was evicted before use (negative training)
  //  - PfSamplerEvict: pf_sampler evicted the entry (negative training)
  enum class SampleEntryType
  {
    Access,
    Evict,
    PfSamplerEvict
  };

  void process_sample_entry(const PfSampler::pf_sampler_entry& entry, SampleEntryType entry_type, uint64_t current_cycle);

  // Convenience wrapper that routes a PfSampler::EvictionEvent to the right
  // SampleEntryType.
  void process_eviction_event(const PfSampler::EvictionEvent& event, uint64_t current_cycle);

  bool should_prefetch(champsim::address access_pc, std::size_t prefetcher_id);

  // For unit tests
  utility_tracker_entry& get_entry(champsim::address pc, std::size_t prefetcher_id);
  const utility_tracker_entry& get_entry(champsim::address pc, std::size_t prefetcher_id) const;

private:
  std::size_t _num_prefetchers{0};
  uint64_t _filter_stats_events{0};

  FilterTable _filter_table;

  static constexpr float SHOULD_PF_THRESHOLD = 0.25;

  // Decay parameters
  static constexpr bool ENABLE_FILTER_STATS_RESET = true;
  static constexpr uint64_t FILTER_STATS_RESET_INTERVAL = 8192;
  static constexpr double FILTER_STATS_DECAY_FACTOR = 0.5;
  static constexpr double FILTER_STATS_RESET_THRESHOLD = 0.01;

  void maybe_reset_filter_stats();
};

#endif
