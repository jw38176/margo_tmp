#ifndef PUMPKIN_CONTROLLER_H
#define PUMPKIN_CONTROLLER_H

#include <vector>

#include "../sampler/sampler.hh"
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

  enum class SampleEntryType
  {
    Access,
    Evict
  };

  void process_sample_entry(const Sampler::sampler_entry& entry, SampleEntryType entry_type, uint64_t current_cycle);

  bool should_prefetch(champsim::address access_pc, std::size_t prefetcher_id);

private:
  std::size_t _num_prefetchers{0};

  FilterTable _filter_table;

  static constexpr float SHOULD_PF_THRESHOLD = 0.5;
};

#endif
