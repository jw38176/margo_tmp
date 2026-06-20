//=======================================================================================//
// File             : pythia/pythia.h
// Author           : Rahul Bera, SAFARI Research Group (write2bera@gmail.com)
// Date             : 20/AUG/2025
// Description      : Implements Pythia prefectcher (Bera+, MICRO'21)
//                    Adapted for ChampSim (CACHE:: free function interface)
//=======================================================================================//

#ifndef __PYTHIA_H__
#define __PYTHIA_H__

#include <cstdint>
#include <deque>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "learning_engine_featurewise.h"
#include "modules.h"
#include "pythia_helper.h"

class CACHE;

struct PythiaPrefetcher {
  std::deque<Scooby_STEntry*> signature_table;
  LearningEngineFeaturewise* brain_featurewise = nullptr;
  std::deque<Scooby_PTEntry*> prefetch_tracker;
  Scooby_PTEntry* last_evicted_tracker = nullptr;

  std::vector<int32_t> Actions;

  PythiaStats stats = {};

  void initialize();
  std::vector<uint64_t> prefetch_candidates(uint64_t address, uint64_t pc, uint8_t dram_bw);

  void init_knobs();
  void update_global_state(uint64_t pc, uint64_t page, uint32_t offset, uint64_t address);
  Scooby_STEntry* update_local_state(uint64_t pc, uint64_t page, uint32_t offset, uint64_t address);
  uint32_t predict(uint64_t address, uint64_t page, uint32_t offset, State* state, std::vector<uint64_t>& pref_addr);
  bool track(uint64_t address, State* state, uint32_t action_index, Scooby_PTEntry** tracker);
  void reward(uint64_t address);
  void reward(Scooby_PTEntry* ptentry);
  void assign_reward(Scooby_PTEntry* ptentry, RewardType type);
  int32_t compute_reward(Scooby_PTEntry* ptentry, RewardType type);
  void train(Scooby_PTEntry* curr_evicted, Scooby_PTEntry* last_evicted);
  void register_fill(uint64_t address);
  std::vector<Scooby_PTEntry*> search_pt(uint64_t address, bool search_all = false);
  void track_in_st(uint64_t page, uint32_t pred_offset, int32_t pref_offset);
  void gen_multi_degree_pref(uint64_t page, uint32_t offset, int32_t action, uint32_t pref_degree, std::vector<uint64_t>& pref_addr);
  uint32_t get_dyn_pref_degree(float max_to_avg_q_ratio, uint64_t page = 0xdeadbeef, int32_t action = 0);
  int32_t getAction(uint32_t action_index);
  bool is_high_bw(uint8_t bw_level);
};

struct pythia : public champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

private:
  PythiaPrefetcher _prefetcher;
};

#endif /* __PYTHIA_H__ */
