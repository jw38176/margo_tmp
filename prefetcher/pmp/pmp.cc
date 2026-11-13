#include "pmp.h"

#include <iostream>
#include <memory>

#include "cache.h"
#include "champsim.h"

void pmp::prefetcher_initialize()
{
  pf = std::make_unique<pmp_space::PMP>(pmp_space::PATTERN_LEN, pmp_space::OFFSET_BITS, pmp_space::OPT_SIZE, pmp_space::OFFSET_MAX_CONF, pmp_space::OPT_WAYS,
                                        pmp_space::PC_BITS, pmp_space::PPT_SIZE, pmp_space::PC_MAX_CONF, pmp_space::PPT_WAYS, pmp_space::FT_SIZE, pmp_space::FT_WAY,
                                        pmp_space::AT_SIZE, pmp_space::AT_WAY, pmp_space::PF_BUFFER_SIZE, pmp_space::PF_BUFFER_WAY, pmp_space::DEBUG_LEVEL,
                                        intern_->cpu);

  std::cout << intern_->NAME << " PMP prefetcher" << std::endl;
}

void pmp::operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type)
{
  (void)cache_hit;
  (void)useful_prefetch;

  if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
    return;
  }

  const uint64_t block_number = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;

  pf->access(block_number, ip.to<uint64_t>());

  if (_collect_candidates) {
    // Candidate mode: the parent meta-prefetcher (Margo) decides which candidates are actually
    // issued. Defer consuming the pattern to notify_result so PMP only marks an offset as prefetched
    // when it was really issued, and un-issued offsets stay available for a later access.
    const auto has_capacity = [] { return true; };
    const auto issue = [this](uint64_t pf_addr, bool fill_this_level, uint32_t metadata, uint64_t region_number, int pf_offset) {
      _candidates.push_back({champsim::address{pf_addr}, _trigger_ip, fill_this_level, metadata,
                             [this, region_number, pf_offset](bool issued, bool /*filtered_out*/) {
                               if (issued) {
                                 pf->clear_offset(region_number, pf_offset);
                               }
                             }});
      return true; // a candidate was produced; commit is deferred to notify_result
    };
    pf->prefetch(block_number, has_capacity, issue, LOG2_BLOCK_SIZE, /*defer_commit=*/true);
  } else {
    const auto has_capacity = [this] {
      return intern_->get_pq_occupancy().back() < intern_->get_pq_size().back()
             && (intern_->get_pq_occupancy().back() + intern_->get_mshr_occupancy()) < (intern_->get_mshr_size() - 1);
    };
    const auto issue = [this](uint64_t pf_addr, bool fill_this_level, uint32_t metadata, uint64_t /*region_number*/, int /*pf_offset*/) {
      return prefetch_line(champsim::address{pf_addr}, fill_this_level, metadata);
    };
    pf->prefetch(block_number, has_capacity, issue, LOG2_BLOCK_SIZE);
  }
}

uint32_t pmp::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                       uint32_t metadata_in)
{
  operate(addr, ip, cache_hit, useful_prefetch, type);
  return metadata_in;
}

uint32_t pmp::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  (void)addr;
  (void)set;
  (void)way;
  (void)prefetch;

  if (evicted_addr != champsim::address{}) {
    const uint64_t evicted_block_number = evicted_addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
    pf->eviction(evicted_block_number);
  }

  return metadata_in;
}

void pmp::prefetcher_final_stats()
{
  std::cout << "Prefetch Request to L1: " << pmp_space::prefetch_to_l1 << ", Prefetch Request to L2: " << pmp_space::prefetch_to_l2 << std::endl;
  std::cout << "Invalid by Eviction: " << pf->invalid_by_eviction << std::endl;
  std::cout << "Invalid by Max: " << pf->invalid_by_max << std::endl;
  std::cout << "Filter by PPT: " << pmp_space::filter_by_ppt << std::endl;
}

std::vector<pmp::Candidate> pmp::prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                                     uint32_t metadata_in)
{
  (void)metadata_in;
  _collect_candidates = true;
  _candidates.clear();
  _trigger_ip = ip;
  operate(addr, ip, cache_hit, useful_prefetch, type);
  _collect_candidates = false;
  return std::move(_candidates);
}
