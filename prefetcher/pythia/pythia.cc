//=======================================================================================//
// File             : pythia/pythia.cc
// Author           : Rahul Bera, SAFARI Research Group (write2bera@gmail.com)
// Date             : 20/AUG/2025
// Description      : Implements Pythia prefectcher (Bera+, MICRO'21)
//                    Adapted for ChampSim (CACHE:: free function interface)
//=======================================================================================//

#include "pythia.h"

#include "pythia_params.h"

void PythiaPrefetcher::initialize()
{
  if (brain_featurewise != nullptr) {
    delete brain_featurewise;
    brain_featurewise = nullptr;
  }

  Actions.clear();
  init_knobs();
  last_evicted_tracker = nullptr;
  brain_featurewise = new LearningEngineFeaturewise(PYTHIA::alpha, PYTHIA::gamma, PYTHIA::epsilon, static_cast<uint32_t>(Actions.size()), PYTHIA::seed,
                                                    PYTHIA::policy, PYTHIA::learning_type);
}

std::vector<uint64_t> PythiaPrefetcher::prefetch_candidates(uint64_t address, uint64_t pc, uint8_t dram_bw)
{
  uint64_t page = address >> LOG2_PAGE_SIZE;
  uint32_t offset = static_cast<uint32_t>((address >> LOG2_BLOCK_SIZE) & ((1ull << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE)) - 1));

  std::vector<uint64_t> pref_addr;

  reward(address);

  update_global_state(pc, page, offset, address);
  Scooby_STEntry* stentry = update_local_state(pc, page, offset, address);

  State* state = new State();
  state->pc = pc;
  state->address = address;
  state->page = page;
  state->offset = offset;
  state->delta = !stentry->deltas.empty() ? stentry->deltas.back() : 0;
  state->local_delta_sig2 = stentry->get_delta_sig2();
  state->local_pc_sig = stentry->get_pc_sig();
  state->local_offset_sig = stentry->get_offset_sig();
  state->is_high_bw = is_high_bw(dram_bw);

  predict(address, page, offset, state, pref_addr);
  return pref_addr;
}

void pythia::prefetcher_initialize()
{
  _prefetcher.initialize();
}

uint32_t pythia::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                          uint32_t metadata_in)
{
  for (auto pf_addr : _prefetcher.prefetch_candidates(addr.to<uint64_t>(), ip.to<uint64_t>(), get_dram_bw())) {
    prefetch_line(champsim::address{pf_addr}, true, metadata_in);
  }

  return metadata_in;
}

uint32_t pythia::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  _prefetcher.register_fill(addr.to<uint64_t>());
  return metadata_in;
}

void pythia::prefetcher_cycle_operate() {}

void pythia::prefetcher_final_stats() {}
