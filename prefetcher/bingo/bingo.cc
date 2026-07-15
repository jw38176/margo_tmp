#include "bingo.h"

#include <iostream>

#include "cache.h"
#include "champsim.h"

void bingo::prefetcher_initialize()
{
  const int pattern_len = bingo_space::REGION_SIZE >> LOG2_BLOCK_SIZE;

  pf = std::make_unique<bingo_space::Bingo>(pattern_len, bingo_space::MIN_ADDR_WIDTH, bingo_space::MAX_ADDR_WIDTH, bingo_space::PC_WIDTH, bingo_space::FT_SIZE,
                                            bingo_space::FT_WAY, bingo_space::AT_SIZE, bingo_space::AT_WAY, bingo_space::PHT_SIZE, bingo_space::PHT_WAY,
                                            bingo_space::PB_SIZE, bingo_space::PB_WAY, bingo_space::DEBUG_LEVEL);

  std::cout << intern_->NAME << " Bingo prefetcher initialised" << std::endl;
}

void bingo::operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type)
{
  (void)cache_hit;
  (void)useful_prefetch;

  if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
    return;
  }

  const uint64_t block_number = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;

  pf->access(block_number, ip.to<uint64_t>());

  if (_collect_candidates) {
    // Candidate mode: the parent meta-prefetcher (naive/alecto/ipop/...) decides which candidates are
    // actually issued. Defer consuming the pattern to notify_result so Bingo only marks an offset as
    // prefetched when it was really issued, and un-issued offsets stay available for a later access.
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

uint32_t bingo::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                         uint32_t metadata_in)
{
  operate(addr, ip, cache_hit, useful_prefetch, type);
  return metadata_in;
}

uint32_t bingo::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
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

void bingo::prefetcher_final_stats() {}

std::vector<bingo::Candidate> bingo::prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                         access_type type, uint32_t metadata_in)
{
  (void)metadata_in;
  _collect_candidates = true;
  _candidates.clear();
  _trigger_ip = ip;
  operate(addr, ip, cache_hit, useful_prefetch, type);
  _collect_candidates = false;
  return std::move(_candidates);
}
