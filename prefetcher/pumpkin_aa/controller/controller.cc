#include "controller.hh"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>

#include "../pumpkin_aa_masking.hh"
#include "cache.h"

namespace pumpkin_aa_detail
{


/////////////////////////////////////////////////////////////
// Filter Table
/////////////////////////////////////////////////////////////

FilterTable::FilterTable(std::size_t size, std::size_t num_prefetchers)
    : _size(size), _num_prefetchers(num_prefetchers), _filter_table(size, std::vector<utility_tracker_entry>(num_prefetchers, utility_tracker_entry{}))
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

utility_tracker_entry& FilterTable::get(champsim::address pc, std::size_t prefetcher_id) { return _filter_table[getIndex(pc)][prefetcher_id]; }

const utility_tracker_entry& FilterTable::get(champsim::address pc, std::size_t prefetcher_id) const { return _filter_table[getIndex(pc)][prefetcher_id]; }

std::vector<std::size_t> FilterTable::rank_prefetchers_by_absolute_accuracy(champsim::address pc) const
{
  std::vector<std::size_t> prefetcher_ids(_num_prefetchers);
  std::iota(prefetcher_ids.begin(), prefetcher_ids.end(), std::size_t{0});

  std::stable_sort(prefetcher_ids.begin(),
                   prefetcher_ids.end(),
                   [this, pc](std::size_t a, std::size_t b)
                   {
                     const uint64_t acc_a = get(pc, a).absolute_accuracy.get_value();
                     const uint64_t acc_b = get(pc, b).absolute_accuracy.get_value();
                     return acc_a > acc_b;
                   });

  return prefetcher_ids;
}

void FilterTable::print_stats(std::ostream& os, const std::function<const char*(std::size_t)>& prefetcher_name) const
{
  os << std::endl << "PUMPKIN Filter Table" << std::endl;
  os << "  table_index  prefetcher  marginal_utility  absolute_accuracy  criticality" << std::endl;
  for (std::size_t idx = 0; idx < _size; ++idx)
  {
    for (std::size_t pf_id = 0; pf_id < _num_prefetchers; ++pf_id)
    {
      const utility_tracker_entry& entry = _filter_table[idx][pf_id];
      os << "  " << idx << "  " << prefetcher_name(pf_id) << "  " << entry.marginal_utility.get_value() << "  " << entry.absolute_accuracy.get_value() << "  "
         << entry.criticality.get_value() << std::endl;
    }
  }
}

/////////////////////////////////////////////////////////////
// Coverage Filter
/////////////////////////////////////////////////////////////

CoverageFilter::CoverageFilter(std::size_t size) : _size(size), _coverage_filter(size, coverage_entry{})
{
  // Size must be a power of 2
  assert(champsim::msl::is_power_of_2(_size));
}

CoverageFilter::~CoverageFilter() = default;

std::size_t CoverageFilter::getIndex(champsim::address pc) const
{
  uint64_t pc_value = pc.to<uint64_t>();
  return (pc_value ^ (pc_value / _size)) % _size;
}

coverage_entry& CoverageFilter::get(champsim::address pc) { return _coverage_filter[getIndex(pc)]; }

const coverage_entry& CoverageFilter::get(champsim::address pc) const { return _coverage_filter[getIndex(pc)]; }

void CoverageFilter::print_stats(std::ostream& os) const
{
  os << std::endl << "PUMPKIN Coverage Filter" << std::endl;
  os << "  table_index  coverage_utility" << std::endl;
  for (std::size_t idx = 0; idx < _size; ++idx)
  {
    os << "  " << idx << "  " << _coverage_filter[idx].coverage_utility.get_value() << std::endl;
  }
}

/////////////////////////////////////////////////////////////
// Controller
/////////////////////////////////////////////////////////////

Controller::Controller(std::size_t num_prefetchers, std::size_t filter_table_size, const std::function<const char*(std::size_t)>& prefetcher_name)
    : _filter_table(filter_table_size, num_prefetchers), _coverage_filter(pumpkin_aa_params::COVERAGE_FILTER_SIZE), _num_prefetchers(num_prefetchers)
{
  // Detect the Triangel prefetcher id by name so its prefetches can be excluded
  // from coverage-utility accounting.
  if (prefetcher_name)
  {
    for (std::size_t id = 0; id < _num_prefetchers; ++id)
    {
      const char* name = prefetcher_name(id);
      if (name != nullptr && std::strcmp(name, "triangel") == 0)
      {
        _triangel_id = id;
        break;
      }
    }
  }
}

Controller::~Controller() = default;

bool Controller::in_llc(champsim::address addr) const { return _llc != nullptr && _llc->in_cache(addr); }

void Controller::process_eviction_event(const PfSampler::EvictionEvent& event, uint64_t current_cycle)
{
  const PfSampler::pf_sampler_entry& entry = event.entry;
  const uint64_t& fill_latency = entry.fill_latency;
  const std::vector<uint64_t>& prefetch_timestamps = entry.prefetch_timestamps;
  const std::vector<champsim::address>& prefetch_src_pcs = entry.prefetch_src_pcs;
  const uint64_t& demand_timestamp = entry.demand_timestamp;
  const uint64_t& first_request_timestamp = entry.first_request_timestamp;

  switch (event.cause)
  {

  /*
  EVICTION PATH:
  Both cache-side and pf_sampler-side evictions land here. The pf_sampler is sized
  so that, in steady state, a PfSamplerCapacity eviction is the calibrated stand-in
  for the cache evict that would have happened had the line actually been prefetched.
  Mark every contributing prefetcher as an inaccurate prefetch.
  */
  case PfSampler::EvictionCause::CacheEvict:
  case PfSampler::EvictionCause::PfSamplerCapacity:
  {
    struct contributor
    {
      std::size_t prefetcher_id;
      uint64_t absolute_accuracy;
    };

    std::vector<contributor> contributors;
    contributors.reserve(_num_prefetchers);

    for (std::size_t i = 0; i < _num_prefetchers; i++)
    {
      // Skip prefetchers that did not prefetch for this address
      if (prefetch_timestamps[i] == 0)
      {
        assert(prefetch_src_pcs[i] == champsim::address{});
        continue;
      }

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);

      uint64_t abs_acc = filter_table_entry.absolute_accuracy.get_value();
      contributors.push_back({i, abs_acc});
    }

    if (contributors.empty())
      return;

    // Sort contributors by absolute accuracy. Ties broken by earlier
    // timestamp so the more timely prefetch is held responsible.
    std::sort(contributors.begin(),
              contributors.end(),
              [&](const contributor& a, const contributor& b)
              {
                if (a.absolute_accuracy != b.absolute_accuracy)
                  return a.absolute_accuracy > b.absolute_accuracy;
                return pumpkin_aa_masking::timestamp_delta(prefetch_timestamps[a.prefetcher_id], first_request_timestamp)
                       < pumpkin_aa_masking::timestamp_delta(prefetch_timestamps[b.prefetcher_id], first_request_timestamp);
              });

    for (const contributor& c : contributors)
    {
      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[c.prefetcher_id], c.prefetcher_id);

      // Treat as inaccurate prefetch
      filter_table_entry.absolute_accuracy.decrement();
    }

    {
      // Penalize a wasted fetch more when it came from DRAM than from the LLC.
      // For a filled line the origin is the cache's ground-truth from_dram bit.
      // Non-issued or still-in-flight prefetches never filled, so no fill-origin
      // bit exists; approximate by the LLC residency probed eagerly at record
      // time (resident -> LLC, cheap to refetch; otherwise assume DRAM). The
      // truncated line tag can no longer probe the LLC directly.
      const bool from_dram = (fill_latency == 0) ? !entry.llc_resident_at_record : entry.from_dram;

      // Instrument the negative path: count this inaccurate prefetch by fill
      // origin (DRAM vs LLC) and whether it had a measured fill latency.
      if (from_dram)
      {
        if (fill_latency == 0)
          _inaccurate_dram_fill_zero++;
        else
          _inaccurate_dram_fill_nonzero++;
      }
      else
      {
        if (fill_latency == 0)
          _inaccurate_llc_fill_zero++;
        else
          _inaccurate_llc_fill_nonzero++;
      }

      const uint64_t marginal_utility_decrements = from_dram ? pumpkin_aa_params::DRAM_PENALTY : pumpkin_aa_params::LLC_PENALTY;

      const contributor& most_accurate = contributors.front();
      utility_tracker_entry& most_accurate_entry = _filter_table.get(prefetch_src_pcs[most_accurate.prefetcher_id], most_accurate.prefetcher_id);

      // Only penalize the most accurate contributor if it is above the marginal-utility threshold
      if (most_accurate_entry.marginal_utility.get_value() >= pumpkin_aa_params::MARGINAL_UTILITY_THRESHOLD)
      {
        for (uint64_t dec = 0; dec < marginal_utility_decrements; ++dec)
          most_accurate_entry.marginal_utility.decrement(1, 1); // 100% probability of decrement
      }
      else
      {
        for (const contributor& c : contributors)
        {
          utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[c.prefetcher_id], c.prefetcher_id);

          for (uint64_t dec = 0; dec < marginal_utility_decrements; ++dec)
            filter_table_entry.marginal_utility.decrement(1, 1); // 100% probability of decrement
        }
      }
    }

    return;
  } // case CacheEvict or PfSamplerCapacity

  /*
  ACCESS PATH:
  Positive training: the demand consumed (or would have consumed) the prefetched line.
  */
  case PfSampler::EvictionCause::CacheHit:
  case PfSampler::EvictionCause::LatePrefetch:
  {
    const champsim::address& demand_pc = entry.demand_pc;

    struct contributor
    {
      std::size_t prefetcher_id;
      uint64_t absolute_accuracy;
    };

    std::vector<contributor> contributors;
    contributors.reserve(_num_prefetchers);

    // If the fill latency is 0, it is either redundant or a late prefetch with delay_subtimely_training disabled.
    if (fill_latency == 0)
      return;

    // Gather prefetchers that contributed to the access
    for (std::size_t i = 0; i < _num_prefetchers; i++)
    {
      // Skip prefetchers that did not prefetch for this address
      if (prefetch_timestamps[i] == 0)
      {
        assert(prefetch_src_pcs[i] == champsim::address{});
        continue;
      }

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[i], i);

      // Accurate prefetch - update filter table entry
      filter_table_entry.absolute_accuracy.increment();

      uint64_t abs_acc = filter_table_entry.absolute_accuracy.get_value();
      contributors.push_back({i, abs_acc});
    }

    if (contributors.empty())
      return;

    // Sort contributors by absolute accuracy. Ties broken by earlier
    // timestamp so the more timely prefetch gets credit first.
    std::sort(contributors.begin(), contributors.end(),
              [&](const contributor& a, const contributor& b)
              {
                if (a.absolute_accuracy != b.absolute_accuracy)
                  return a.absolute_accuracy > b.absolute_accuracy;
                return pumpkin_aa_masking::timestamp_delta(prefetch_timestamps[a.prefetcher_id], first_request_timestamp)
                       < pumpkin_aa_masking::timestamp_delta(prefetch_timestamps[b.prefetcher_id], first_request_timestamp);
              });

    // Marginal Utility Calculation
    uint64_t reference_time;
    uint64_t latency_credit;

    // Determine reference time and available latency credit
    if (event.cause == PfSampler::EvictionCause::CacheHit)
    {
      // Timely prefetches
      // Reference time is the demand timestamp
      reference_time = demand_timestamp;
      // Full latency credit is available
      latency_credit = fill_latency;
    }
    else
    {
      // Late prefetches
      // Reference time is the demand timestamp (fill happens later)
      reference_time = demand_timestamp;
      // Only the latency porentially hidden by the prefetch is available to credit
      latency_credit = pumpkin_aa_masking::timestamp_delta(demand_timestamp, first_request_timestamp);
    }

    // Get criticality of the demand
    uint64_t criticality = _filter_table.get(demand_pc, 0).criticality.get_value();

    for (const contributor& c : contributors)
    {
      const uint64_t prefetch_time = prefetch_timestamps[c.prefetcher_id];

      // A prefetch earns credit only if it plausibly preceded the reference time
      // (wrap-safe comparison of the masked timestamps). Late or dominated
      // prefetches (not before the reference) are skipped rather than asserted on,
      // since a masked timestamp can no longer be ordered by raw magnitude.
      if (!pumpkin_aa_masking::timestamp_before(prefetch_time, reference_time))
        continue;

      // Compute latency hidden credit
      uint64_t latency_hidden = std::min(pumpkin_aa_masking::timestamp_delta(reference_time, prefetch_time), latency_credit);

      utility_tracker_entry& filter_table_entry = _filter_table.get(prefetch_src_pcs[c.prefetcher_id], c.prefetcher_id);

      // Scale marginal utility by fill origin (LLC or DRAM). fill_latency != 0 is
      // guaranteed here (the fill_latency == 0 early-return above), so the
      // ground-truth from_dram bit is valid.
      uint64_t marginal_utility_increments = entry.from_dram ? pumpkin_aa_params::DRAM_REWARD : pumpkin_aa_params::LLC_REWARD;

      // Define fractional marginal utility
      uint64_t numerator = latency_hidden;
      uint64_t denominator = fill_latency;

      // Scale marginal utility by criticality
      // numerator *= criticality;
      // denominator *= (1 << (pumpkin_aa_params::CRITICALITY_BITS - 1)); // Initialisation value

      for (uint64_t inc = 0; inc < marginal_utility_increments; ++inc)
        filter_table_entry.marginal_utility.increment(numerator, denominator);

      latency_credit -= latency_hidden;
      reference_time = (reference_time - latency_hidden) & pumpkin_aa_params::TIMESTAMP_MASK;

      const bool loop_break = latency_credit == 0;

      if (loop_break)
        break;
    }

    // Coverage Utility update. Measure the fraction of this covered load's
    // latency that was hidden by the most timely issuable non-Triangel
    // prefetch, then step the PC's CU counter toward that observed fraction.
    // If no non-Triangel prefetcher covered it, the observed fraction is 0 and
    // the counter is decremented (negative coverage).
    {
      uint64_t earliest_nontriangel_ts = 0;
      uint64_t earliest_nontriangel_delta = 0; // gap from first_request_timestamp; smallest == earliest
      bool covered_by_nontriangel = false;

      for (std::size_t i = 0; i < _num_prefetchers; ++i)
      {
        if (i == _triangel_id)
          continue;
        if (prefetch_timestamps[i] == 0)
          continue;
        // Only count accurate (issuable) prefetches as coverage.
        if (!should_prefetch(prefetch_src_pcs[i], i))
          continue;

        // Anchor ordering on first_request_timestamp so the earliest prefetch is
        // recovered even though the raw timestamps are masked (wrap-safe).
        const uint64_t delta = pumpkin_aa_masking::timestamp_delta(prefetch_timestamps[i], first_request_timestamp);
        if (!covered_by_nontriangel || delta < earliest_nontriangel_delta)
        {
          earliest_nontriangel_ts = prefetch_timestamps[i];
          earliest_nontriangel_delta = delta;
          covered_by_nontriangel = true;
        }
      }

      uint64_t covered_latency = 0;
      if (covered_by_nontriangel && pumpkin_aa_masking::timestamp_before(earliest_nontriangel_ts, demand_timestamp))
        covered_latency = std::min(pumpkin_aa_masking::timestamp_delta(demand_timestamp, earliest_nontriangel_ts), fill_latency);

      _coverage_filter.get(demand_pc).coverage_utility.update_toward(covered_latency, fill_latency);
    }

    break;

  } // case CacheHit or LatePrefetch
  }
}

void Controller::process_criticality_entry(const CritSampler::crit_sampler_entry& entry)
{

  // No measured fetch latency (e.g. the line was already resident at the demand,
  // so no fetch occurred). A prefetch could not have hidden any latency here, so
  // the sample carries no criticality signal in either direction; ignore it.
  if (entry.natural_latency == 0)
    return;

  // Counterfactual criticality. Had this load missed instead of being (pre)fetched,
  // its data could not have arrived before demand_issue_time + natural_latency, and
  // the instruction could not have reached the ROB head before then. If it
  // nonetheless reached the head earlier than that, the fetch latency was on the
  // critical path and the (pre)fetch genuinely pulled the ROB-head event in ->
  // critical. This is invariant to whether the line was actually prefetched, which
  // is what breaks the hysteresis: a successful prefetch that turns the demand into
  // an L1 hit no longer hides the load's criticality.
  // Wrap-safe elapsed time from demand issue to ROB head. The masked timestamps
  // can no longer be ordered by raw magnitude, so a modular delta is used; an
  // implausibly large value (a wrap artifact) is treated as non-critical.
  const uint64_t rob_head_since_issue = pumpkin_aa_masking::timestamp_delta(entry.rob_head_time, entry.demand_issue_time);

  const bool critical = rob_head_since_issue <= pumpkin_aa_params::TIMESTAMP_HALF && rob_head_since_issue < entry.natural_latency;

  utility_tracker_entry& filter_table_entry = _filter_table.get(entry.trigger_pc, 0);

  if (critical)
  {
    _critical_loads_recorded++;
    filter_table_entry.criticality.increment();
  }
  else
  {
    _non_critical_loads_recorded++;
    filter_table_entry.criticality.decrement();
  }
}

void Controller::process_criticality_event(const CritSampler::EvictionEvent& event) { process_criticality_entry(event.entry); }

void Controller::print_filter_table_stats(std::ostream& os, const std::function<const char*(std::size_t)>& prefetcher_name) const
{
  _filter_table.print_stats(os, prefetcher_name);
}

void Controller::print_coverage_filter_stats(std::ostream& os) const { _coverage_filter.print_stats(os); }

void Controller::decrease_cu(champsim::address demand_pc) { _coverage_filter.get(demand_pc).coverage_utility.decrement(); }

bool Controller::should_track_with_triangel(champsim::address pc, bool is_miss) const
{

  // return true; // CU OFF ABALATION
  const ssc& coverage_utility = _coverage_filter.get(pc).coverage_utility;
  const uint64_t inverse_coverage = coverage_utility.saturation_value - coverage_utility.get_value();

  return inverse_coverage > pumpkin_aa_params::TEST_THRESHOLD;
}

bool Controller::should_prefetch(champsim::address access_pc, std::size_t prefetcher_id)
{
  // const uint64_t absolute_accuracy = _filter_table.get(access_pc, prefetcher_id).absolute_accuracy.get_value();

  // return absolute_accuracy >= pumpkin_aa_params::MARGINAL_UTILITY_THRESHOLD;

  // uint64_t marginal_utility = _filter_table.get(access_pc, prefetcher_id).marginal_utility.get_value();
  // ABLATION: MU OFF
  uint64_t absolute_accuracy = _filter_table.get(access_pc, prefetcher_id).absolute_accuracy.get_value();
  if (absolute_accuracy >= pumpkin_aa_params::MARGINAL_UTILITY_THRESHOLD)
    return true;
  // if (marginal_utility >= pumpkin_aa_params::MARGINAL_UTILITY_THRESHOLD)
  //   return true;
  // return false;
}

std::vector<std::size_t> Controller::rank_prefetchers_by_absolute_accuracy(champsim::address pc) const
{
  return _filter_table.rank_prefetchers_by_absolute_accuracy(pc);
}

} // namespace pumpkin_aa_detail
