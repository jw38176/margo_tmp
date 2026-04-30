#include "controller.hh"

/////////////////////////////////////////////////////////////
// Filter Table
/////////////////////////////////////////////////////////////


FilterTable::FilterTable(std::size_t size, std::size_t num_prefetchers): 
_size(size), 
_num_prefetchers(num_prefetchers), 
_table(size, std::vector<utility_tracker_entry>(num_prefetchers, utility_tracker_entry{})) 
{}

FilterTable::~FilterTable() = default;

std::size_t FilterTable::getIndex(champsim::address pc) const {
  uint64_t pc_value = pc.to<uint64_t>();
  return (pc_value ^ (pc_value / _size)) % _size;
}

utility_tracker_entry& FilterTable::get(champsim::address pc, std::size_t prefetcher_id) {
  return _table[getIndex(pc)][prefetcher_id];
}

/////////////////////////////////////////////////////////////
// Controller
/////////////////////////////////////////////////////////////

Controller::Controller(std::size_t num_prefetchers, std::size_t filter_table_size): 
_num_prefetchers(num_prefetchers),
_filter_table(filter_table_size, num_prefetchers) 
{}

Controller::~Controller() = default;

void Controller::process_sample_entry(const Sampler::sampler_entry& entry, SampleEntryType entry_type)
{
  // Unpack the sampler entry
  const champsim::address& pf_line_addr = entry.pf_line_addr;
  const uint64_t& fill_latency = entry.fill_latency;
  const uint64_t& first_used_timestamp = entry.first_used_timestamp;
  const std::vector<uint64_t>& prefetch_timestamps = entry.prefetch_timestamps;
  const std::vector<champsim::address>& prefetch_src_pcs = entry.prefetch_src_pcs;

  for (std::size_t i = 0; i < _num_prefetchers; i++) {

    // Only update if prefetched
    if (prefetch_timestamps[i] == 0) {
      assert(prefetch_src_pcs[i] == champsim::address{});
      continue;
    }

    // Get the filter table entry
    utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);

    // Update the filter table entry
    switch (entry_type) {
    case SampleEntryType::Access:
      filter_table_entry.marginal_utility += 1;
      filter_table_entry.acc_pf_count += 1;
      break;
    case SampleEntryType::Evict:
      filter_table_entry.inacc_pf_count += 1;
      break;
    }
  }
}

bool Controller::should_prefetch(champsim::address access_pc, std::size_t prefetcher_id)
{
  // Get the filter table entry
  utility_tracker_entry& filter_table_entry = _filter_table.get(access_pc, prefetcher_id);

  // Calculate the prefetch score
  float prefetch_score = 
  static_cast<float>(filter_table_entry.marginal_utility) 
  / static_cast<float>(filter_table_entry.inacc_pf_count + filter_table_entry.marginal_utility);

  return prefetch_score > SHOULD_PF_THRESHOLD;
}
