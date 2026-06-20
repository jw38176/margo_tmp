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

void FilterTable::decay_stats(double decay_factor, double reset_threshold)
{
  for (auto& pc_entries : _table)
  {
    for (utility_tracker_entry& entry : pc_entries)
    {
      entry.marginal_utility *= decay_factor;
      entry.acc_pf_count /= 2;
      entry.inacc_pf_count /= 2;

      if (entry.marginal_utility < reset_threshold && entry.acc_pf_count == 0 && entry.inacc_pf_count == 0)
      {
        entry = {};
      }
    }
  }
}

/////////////////////////////////////////////////////////////
// Controller
/////////////////////////////////////////////////////////////

Controller::Controller(std::size_t num_prefetchers, std::size_t filter_table_size)
    : _num_prefetchers(num_prefetchers), _filter_table(filter_table_size, num_prefetchers)
{
}

Controller::~Controller() = default;

void Controller::maybe_reset_filter_stats()
{
  if (!ENABLE_FILTER_STATS_RESET)
    return;

  _filter_stats_events++;
  if (_filter_stats_events < FILTER_STATS_RESET_INTERVAL)
    return;

  _filter_stats_events = 0;
  _filter_table.decay_stats(FILTER_STATS_DECAY_FACTOR, FILTER_STATS_RESET_THRESHOLD);
}

void Controller::process_sample_entry(const PfSampler::pf_sampler_entry& entry, SampleEntryType entry_type, uint64_t current_cycle)
{
  maybe_reset_filter_stats();

  const uint64_t& fill_latency = entry.fill_latency;
  const std::vector<uint64_t>& prefetch_timestamps = entry.prefetch_timestamps;
  const std::vector<champsim::address>& prefetch_src_pcs = entry.prefetch_src_pcs;

  switch (entry_type)
  {

  /*
  EVICTION PATH:
  Both cache-side and pf_sampler-side evictions land here. The pf_sampler is sized
  so that, in steady state, a PfSamplerEvict is the calibrated stand-in for the
  cache evict that would have happened had the line actually been prefetched.
  Mark every contributing prefetcher as an inaccurate prefetch.
  */
  case SampleEntryType::Evict:
  case SampleEntryType::PfSamplerEvict:
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
  Positive training: the demand consumed (or would have consumed) the prefetched line.
  */
  case SampleEntryType::Access:
  {
    struct contributor
    {
      std::size_t prefetcher_id;
      double absolute_accuracy;
    };

    std::vector<contributor> contributors;
    contributors.reserve(_num_prefetchers);

    for (std::size_t i = 0; i < _num_prefetchers; i++)
    {
      if (prefetch_timestamps[i] == 0)
      {
        assert(prefetch_src_pcs[i] == champsim::address{});
        continue;
      }

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);

      // Calculate the absolute accuracy of the prefetcher
      const uint64_t total = filter_table_entry.acc_pf_count + filter_table_entry.inacc_pf_count;
      const double abs_acc = (total == 0) ? 0.0 : static_cast<double>(filter_table_entry.acc_pf_count) / static_cast<double>(total);
      contributors.push_back({i, abs_acc});

      filter_table_entry.acc_pf_count += 1;
    }

    if (contributors.empty() || fill_latency == 0)
      return;

    // Sort contributors by absolute accuracy. Ties broken by earlier
    // timestamp so the more timely prefetch gets first crack at credit.
    std::sort(contributors.begin(), contributors.end(),
              [&](const contributor& a, const contributor& b)
              {
                if (a.absolute_accuracy != b.absolute_accuracy)
                  return a.absolute_accuracy > b.absolute_accuracy;
                return prefetch_timestamps[a.prefetcher_id] < prefetch_timestamps[b.prefetcher_id];
              });

    const uint64_t cov_window_start = current_cycle - fill_latency;

    // earliest_covered marks the earliest cycle in the window already credited by a more accurate prefetch.
    // New prefetches only get credit for cycles before this point.
    uint64_t earliest_covered = current_cycle;

    for (const contributor& c : contributors)
    {
      const std::size_t i = c.prefetcher_id;
      const uint64_t t_p = std::max(prefetch_timestamps[i], cov_window_start);

      if (t_p >= earliest_covered)
      {
        // Already covered by a more accurate prefetcher
        continue;
      }

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);

      const uint64_t new_coverage = earliest_covered - t_p;

      filter_table_entry.marginal_utility += static_cast<double>(new_coverage) / static_cast<double>(fill_latency);

      earliest_covered = t_p;

      if (earliest_covered <= cov_window_start)
        break;
    }
  }
  }
}

void Controller::process_eviction_event(const PfSampler::EvictionEvent& event, uint64_t current_cycle)
{
  switch (event.cause)
  {
  case PfSampler::EvictionCause::CacheHit:
    process_sample_entry(event.entry, SampleEntryType::Access, current_cycle);
    return;
  case PfSampler::EvictionCause::CacheEvict:
    process_sample_entry(event.entry, SampleEntryType::Evict, current_cycle);
    return;
  case PfSampler::EvictionCause::PfSamplerCapacity:
    process_sample_entry(event.entry, SampleEntryType::PfSamplerEvict, current_cycle);
    return;
  }
}

// For unit tests
utility_tracker_entry& Controller::get_entry(champsim::address pc, std::size_t prefetcher_id) { return _filter_table.get(pc, prefetcher_id); }

const utility_tracker_entry& Controller::get_entry(champsim::address pc, std::size_t prefetcher_id) const
{
  return const_cast<FilterTable&>(_filter_table).get(pc, prefetcher_id);
}

bool Controller::should_prefetch(champsim::address access_pc, std::size_t prefetcher_id)
{
  // Get the filter table entry
  utility_tracker_entry& filter_table_entry = _filter_table.get(access_pc, prefetcher_id);

  // Calculate the prefetch score
  const double marg_util = filter_table_entry.marginal_utility;
  const double inacc = static_cast<double>(filter_table_entry.inacc_pf_count);
  const double denom = marg_util + inacc;

  // If the denominator is 0, this means that there is no prefetch history
  // Issue the prefetch regardless to acquire feedback for this PC
  if (denom == 0.0)
    return true;

  const float prefetch_score = static_cast<float>(marg_util / denom);

  return prefetch_score > SHOULD_PF_THRESHOLD;
}
