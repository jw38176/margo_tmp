#ifndef PREFETCHER_CAERUS_H
#define PREFETCHER_CAERUS_H

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "address.h"
#include "caerus_parameters.h"
#include "modules.h"
#include "msl/bits.h"

class caerus;

namespace caerus_space
{

class RRTable
{
public:
  explicit RRTable(std::size_t rr_size, std::size_t rr_entry_size, caerus* owner_arg);

  struct Entry
  {
    uint64_t addr_tag = 0;
    uint64_t pc = 0;
  };

  struct ShiftRegister
  {
    std::deque<Entry> entries;
  };

  Entry getEvict(uint64_t addr_tag);

  Entry insert(uint64_t addr_tag, uint64_t pc);

  bool test(uint64_t addr_tag) const;

  bool test_partial(uint64_t addr_tag) const;

  void remove(uint64_t addr_tag);

  Entry lookup(uint64_t addr_tag) const;

  std::vector<uint64_t> genAllOffsets(uint64_t addr_tag) const;

  bool isSamePage(uint64_t addr_tag) const;

private:
  caerus* owner;
  std::size_t rr_size;       // Number of shift registers
  std::size_t rr_entry_size; // Number of entries in each shift register
  std::vector<ShiftRegister> table;

  std::size_t index(uint64_t addr_tag) const;
};

class HoldingTable
{
public:
  struct Entry
  {
    uint64_t pref_addr_tag = 0; // tag of prefetched line
    uint64_t base_addr_tag = 0; // tag of demand address that triggered the prefetch
    uint64_t pc = 0;
  };

  explicit HoldingTable(std::size_t size, caerus* owner_arg);

  void insert(uint64_t pref_addr_tag, uint64_t base_addr_tag, uint64_t pc);

  std::optional<Entry> lookup(uint64_t pref_addr_tag);

  bool test(uint64_t pref_addr_tag);

  void remove(uint64_t pref_addr_tag);

private:
  caerus* owner;
  std::vector<Entry> entries;   // size == capacity
  std::size_t capacity;         // Maximum number of valid entries
  std::size_t count = 0;        // Current occupancy (<= capacity)
  std::size_t eviction_idx = 0; // Round-robin index for eviction when full
};

class AccuracyTable
{
public:
  explicit AccuracyTable(std::size_t size);

  int16_t lookup(uint64_t pc, std::size_t offset_idx) const;

  void increment(uint64_t pc, std::size_t offset_idx);

  void decrement(uint64_t pc, std::size_t offset_idx);

  void resetOffsetStats(std::size_t offset_idx);

private:
  std::size_t table_size;

  std::vector<std::vector<uint16_t>> table;

  std::vector<std::size_t> activation_table;

  std::size_t getIndex(uint64_t pc) const;

  static constexpr int16_t ACC_MIN = 0;
  static constexpr int16_t ACC_MAX = 15;
};

} // namespace caerus_space

class caerus : public champsim::modules::prefetcher
{
public:
  using prefetcher::prefetcher;

  /** Structure to save the offset and the score */
  using OffsetListEntry = std::pair<int16_t, uint8_t>;

  std::vector<OffsetListEntry> offsetsList;

  std::array<uint64_t, NUM_OFFSETS> learned_offsets{};
  std::size_t current_learning_offset_idx = 0;

  /** Learning phase parameters */
  unsigned int scoreMax = SCORE_MAX;
  unsigned int roundMax = ROUND_MAX;
  unsigned int badScore = BAD_SCORE;

  /** Current best offset found in the learning phase */
  uint64_t phaseBestOffset = 0;
  /** Max score found so far */
  unsigned int bestScore = 0;
  /** Current round */
  unsigned int round = 0;

  bool DEBUG = false;

  // Stats counters
  uint64_t mem_op_counter = 0;
  uint64_t round_max_counter = 0;
  uint64_t score_max_counter = 0;
  uint64_t bad_score_counter = 0;
  uint64_t increment_counter = 0;
  uint64_t decrement_counter = 0;
  uint64_t holding_table_eviction_counter = 0;
  uint64_t subtimely_rr_insertion_counter = 0;
  uint64_t holding_max_occupancy = 0;
  uint64_t duplicate_prefetch_counter = 0;
  uint64_t evict_in_rr_counter = 0;
  uint64_t below_threshold_counter = 0;
  uint64_t general_pf_counter = 0;

  caerus_space::RRTable rr_table{RR_SIZE, RR_ENTRY_SIZE, this};
  caerus_space::HoldingTable holding_table{HOLDING_TABLE_SIZE, this};
  caerus_space::AccuracyTable accuracy_table{ACCURACY_TABLE_SIZE};

  /** Reset all the scores from the offset list */
  void resetScores();

  /** Learning phase of CAERUS. */
  void offsetLearning(uint64_t line_addr, uint8_t cache_hit);

  std::vector<uint64_t> calculateAccuratePrefetchAddrs(uint64_t full_addr, uint64_t line_addr, uint64_t pc);

  std::vector<uint64_t> calculateAllPrefetchAddrs(uint64_t line_addr);

  std::size_t next_training_offset_idx();

  void insertFill(uint64_t line_addr);

  void accuracy_train(uint64_t line_addr, uint64_t pc);

  void rr_insert_and_train_acc(uint64_t line_addr, uint64_t pc);

  void holding_insert(uint64_t pref_line_addr, uint64_t base_line_addr, uint64_t pc);

  uint64_t addr_tag(uint64_t line_addr) const;

  // ChampSim prefetcher interface
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif
