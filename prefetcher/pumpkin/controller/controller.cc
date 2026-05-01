#include "controller.hh"

#include <algorithm>
#include <vector>

/////////////////////////////////////////////////////////////
// Filter Table
/////////////////////////////////////////////////////////////

FilterTable::FilterTable(std::size_t size, std::size_t num_prefetchers)
    : _size(size), _num_prefetchers(num_prefetchers), _table(size, std::vector<utility_tracker_entry>(num_prefetchers, utility_tracker_entry{}))
{
  // Size must be a power of 2
  assert(champsim::msl::is_power_of_2(_size));
}

FilterTable::~FilterTable() = default;

std::size_t FilterTable::getIndex(champsim::address pc) const
{
  uint64_t pc_value = pc.to<uint64_t>();
  return (pc_value ^ (pc_value / _size)) % _size;
}

utility_tracker_entry& FilterTable::get(champsim::address pc, std::size_t prefetcher_id) { return _table[getIndex(pc)][prefetcher_id]; }

/////////////////////////////////////////////////////////////
// Controller
/////////////////////////////////////////////////////////////

Controller::Controller(std::size_t num_prefetchers, std::size_t filter_table_size): 
_num_prefetchers(num_prefetchers),
_filter_table(filter_table_size, num_prefetchers) 
{}

Controller::~Controller() = default;

void Controller::process_sample_entry(const Sampler::sampler_entry& entry, SampleEntryType entry_type, uint64_t current_cycle)
{
  const uint64_t& fill_latency = entry.fill_latency;
  const std::vector<uint64_t>& prefetch_timestamps = entry.prefetch_timestamps;
  const std::vector<champsim::address>& prefetch_src_pcs = entry.prefetch_src_pcs;

  switch (entry_type)
  {

  /*
  EViction PATH:
  Mark prefetcher that touched the line as an inaccurate prefetch.
  */
  case SampleEntryType::Evict:
  {
    for (std::size_t i = 0; i < _num_prefetchers; i++)
    {
      if (prefetch_timestamps[i] == 0)
      {
        assert(prefetch_src_pcs[i] == champsim::address{});
        continue;
      }

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);
      filter_table_entry.inacc_pf_count += 1;
    }

    return;
  }

  /*
  ACCESS PATH:
  Collect every prefetcher that touched the line.
  Each will count as an accurate prefetch
  The fill_latency credit is split based on how recently they issued the prefetch.
  */
  case SampleEntryType::Access:
  {
    std::vector<std::size_t> contributors;
    contributors.reserve(_num_prefetchers);

    for (std::size_t i = 0; i < _num_prefetchers; i++)
    {
      if (prefetch_timestamps[i] == 0)
      {
        assert(prefetch_src_pcs[i] == champsim::address{});
        continue;
      }

      contributors.push_back(i);
      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);
      filter_table_entry.acc_pf_count += 1;
    }

    if (contributors.empty() || fill_latency == 0)
      return;

    std::sort(contributors.begin(), contributors.end(), [&](std::size_t a, std::size_t b) { return prefetch_timestamps[a] > prefetch_timestamps[b]; });

    const uint64_t window_start = (current_cycle > fill_latency) ? current_cycle - fill_latency : 0;

    uint64_t segment_end = current_cycle;

    for (std::size_t i : contributors)
    {

      const uint64_t t_p = std::max(prefetch_timestamps[i], window_start);

      if (t_p >= segment_end)
      {
        // A more recent prefetcher already claimed all remaining cycles.
        break;
      }

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);

      const uint64_t covered = segment_end - t_p;

      /*
      Marginal Utility Calculation:
      The marginal utility is the fraction of the fill latency that this prefetcher hid.
      The fill latency credit is split based on how recently they issued the prefetch.
      */
      filter_table_entry.marginal_utility += static_cast<double>(covered) / static_cast<double>(fill_latency);

      segment_end = t_p;
    }
  }
  }
}

bool Controller::should_prefetch(champsim::address access_pc, std::size_t prefetcher_id)
{
  // Get the filter table entry
  utility_tracker_entry& filter_table_entry = _filter_table.get(access_pc, prefetcher_id);

  // Calculate the prefetch score
  const double marg_util = filter_table_entry.marginal_utility;
  const double inacc = static_cast<double>(filter_table_entry.inacc_pf_count);
  const double denom = marg_util + inacc;

  if (denom == 0.0)
    return false;

  const float prefetch_score = static_cast<float>(marg_util / denom);

  return prefetch_score > SHOULD_PF_THRESHOLD;
}
