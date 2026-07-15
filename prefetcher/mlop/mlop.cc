#include "mlop.h"

#include <iostream>
#include <utility>

#include "cache.h"
#include "champsim.h"

void mlop::prefetcher_initialize()
{
  const int BLOCKS_IN_CACHE = static_cast<int>(intern_->NUM_SET * intern_->NUM_WAY);
  blocks_in_zone = static_cast<int>(PAGE_SIZE / BLOCK_SIZE);
  const int AMT_SIZE = 16 * BLOCKS_IN_CACHE / blocks_in_zone; /* size of access map table */

  pf = std::make_unique<mlop_space::MLOP>(blocks_in_zone, AMT_SIZE, mlop_space::PREFETCH_DEGREE, mlop_space::NUM_UPDATES, mlop_space::L1D_THRESH,
                                          mlop_space::L2C_THRESH, mlop_space::LLC_THRESH, mlop_space::DEBUG_LEVEL);

  std::cout << "MLOP Prefetcher Initialised" << std::endl;
}

void mlop::operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type)
{
  (void)ip;
  if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
    return;
  }

  const uint64_t block_number = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;

  const bool trigger_access = (!cache_hit) || useful_prefetch;
  if (trigger_access) {
    pf->access(block_number);
  }

  pf->mark(block_number, mlop_space::State::ACCESS);

  if (_collect_candidates) {
    // Candidate mode: the parent meta-prefetcher (naive/alecto/ipop/...) decides which candidates are
    // actually issued. Defer the access-map mark to notify_result so MLOP only records a block as
    // prefetched when it was really issued, exactly like the standalone path.
    const auto has_capacity = [] { return true; };
    const auto issue = [this](uint64_t pf_addr, bool fill_this_level, uint32_t metadata, int fill_level) {
      const uint64_t pf_block_number = pf_addr >> LOG2_BLOCK_SIZE;
      _candidates.push_back({champsim::address{pf_addr}, _trigger_ip, fill_this_level, metadata,
                             [this, pf_block_number, fill_level](bool issued, bool /*filtered_out*/) {
                               if (issued) {
                                 pf->mark(pf_block_number, mlop_space::State::PREFTCH, fill_level);
                               }
                             }});
      return false; // defer marking until the meta-prefetcher reports the outcome
    };
    pf->prefetch(block_number, has_capacity, issue);
  } else {
    const auto has_capacity = [this] {
      return intern_->get_pq_occupancy().back() < intern_->get_pq_size().back()
             && (intern_->get_pq_occupancy().back() + intern_->get_mshr_occupancy()) < (intern_->get_mshr_size() - 1);
    };
    const auto issue = [this](uint64_t pf_addr, bool fill_this_level, uint32_t metadata, int /*fill_level*/) {
      return prefetch_line(champsim::address{pf_addr}, fill_this_level, metadata);
    };
    pf->prefetch(block_number, has_capacity, issue);
  }

  /* stats */
  pf->track(block_number);
}

uint32_t mlop::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                        uint32_t metadata_in)
{
  operate(addr, ip, cache_hit, useful_prefetch, type);
  return metadata_in;
}

uint32_t mlop::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr,
                                     uint32_t metadata_in)
{
  (void)addr;
  (void)set;
  (void)way;
  (void)prefetch;

  if (evicted_addr != champsim::address{}) {
    const uint64_t evicted_block_number = evicted_addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
    pf->mark(evicted_block_number, mlop_space::State::INIT);
    /* stats */
    pf->track(evicted_block_number);
  }

  return metadata_in;
}

void mlop::prefetcher_final_stats()
{
  if (pf) {
    std::cout << intern_->NAME << " MLOP Prefetcher Statistics:" << std::endl;
    pf->print_stats();
  }
}

std::vector<mlop::Candidate> mlop::prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
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
