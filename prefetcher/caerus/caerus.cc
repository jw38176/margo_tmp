#include "caerus.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <fmt/core.h>

#include "cache.h"
#include "champsim.h"

using namespace caerus_space;

/////////////////////////////////////////////////////////////
// RR Table
/////////////////////////////////////////////////////////////

RRTable::RRTable(std::size_t size, std::size_t entry_size, caerus* owner_arg) : owner(owner_arg), rr_size(size), rr_entry_size(entry_size) { table.resize(rr_size); }

std::size_t RRTable::index(uint64_t addr_tag) const
{
  // Indexed by page
  uint64_t full_addr = addr_tag << LOG2_BLOCK_SIZE;
  uint64_t page_addr = full_addr >> LOG2_PAGE_SIZE;
  return page_addr % rr_size;
}

RRTable::Entry RRTable::insert(uint64_t addr_tag, uint64_t pc)
{
  if (test(addr_tag)) {
    if (owner->DEBUG)
      fmt::print("[RR_TABLE_INSERT] Skipping due to duplicate entry: {:#x}\n", addr_tag);
    return Entry{};
  }

  std::size_t idx = index(addr_tag);

  table[idx].entries.push_back({addr_tag, pc});

  if (table[idx].entries.size() > rr_entry_size) {
    Entry evicted_entry = table[idx].entries.front();
    table[idx].entries.pop_front();
    return evicted_entry;
  }

  return Entry{};
}

bool RRTable::test(uint64_t addr_tag) const
{
  std::size_t idx = index(addr_tag);
  for (const auto& entry : table[idx].entries) {
    if (entry.addr_tag == addr_tag) {
      return true;
    }
  }
  return false;
}

bool RRTable::test_partial(uint64_t addr_tag) const
// Returns true if the entry is in the last half of the shift register of the region
{
  int x = 0;
  std::size_t idx = index(addr_tag);
  for (const auto& entry : table[idx].entries) {
    x++;
    if (x > (2 * static_cast<int>(rr_entry_size)) / 3)
      return false;
    if (entry.addr_tag == addr_tag) {
      return true;
    }
  }
  return false;
}

RRTable::Entry RRTable::lookup(uint64_t addr_tag) const
{
  std::size_t idx = index(addr_tag);
  for (const auto& entry : table[idx].entries) {
    if (entry.addr_tag == addr_tag) {
      return entry;
    }
  }
  return Entry{};
}

std::vector<uint64_t> RRTable::genAllOffsets(uint64_t addr_tag) const
{
  std::vector<uint64_t> offsets;
  for (const auto& entry : table[index(addr_tag)].entries) {
    if (owner->DEBUG)
      fmt::print("[RR_TABLE_CONTENTS] entry.line_addr: {:#x}\n", entry.addr_tag);
    offsets.push_back(addr_tag - entry.addr_tag);
  }
  return offsets;
}

RRTable::Entry RRTable::getEvict(uint64_t addr_tag) { return table[index(addr_tag)].entries.front(); }

bool RRTable::isSamePage(uint64_t addr_tag) const
{
  uint64_t full_addr = addr_tag << LOG2_BLOCK_SIZE;
  uint64_t page_addr = full_addr >> LOG2_PAGE_SIZE;
  std::size_t idx = index(addr_tag);
  for (const auto& entry : table[idx].entries) {
    uint64_t entry_full_addr = entry.addr_tag << LOG2_BLOCK_SIZE;
    uint64_t entry_page_addr = entry_full_addr >> LOG2_PAGE_SIZE;
    if (entry_page_addr == page_addr) {
      return true;
    }
  }
  return false;
}

/////////////////////////////////////////////////////////////
// Holding Table
/////////////////////////////////////////////////////////////

HoldingTable::HoldingTable(std::size_t size, caerus* owner_arg) : owner(owner_arg), capacity(size) { entries.resize(size); }

void HoldingTable::insert(uint64_t pref_addr_tag, uint64_t base_addr_tag, uint64_t pc)
{
  for (const auto& entry : entries) {
    if (entry.pref_addr_tag == pref_addr_tag) {
      if (owner->DEBUG)
        fmt::print("[HOLDING_INSERT] Skipping due to duplicate prefetch address: {:#x}\n", pref_addr_tag);
      return;
    }
  }

  for (std::size_t i = 0; i < capacity; ++i) {
    Entry& e = entries[i];
    if (e.pref_addr_tag == 0) {
      e = {pref_addr_tag, base_addr_tag, pc};
      count++;
      if (count > owner->holding_max_occupancy) {
        owner->holding_max_occupancy = count;
      }
      if (owner->DEBUG)
        fmt::print("[HOLDING_INSERT] pref_addr_tag: {:#x} base_addr_tag: {:#x} pc: {:#x} size: {}\n", pref_addr_tag, base_addr_tag, pc, count);
      return;
    }
  }

  if (HOLDING_TABLE_ASSERT_ON_FULL) {
    if (owner->DEBUG)
      fmt::print("[HOLDING_TABLE_CONTENTS] size: {}\n", count);
    for (std::size_t i = 0; i < capacity; ++i) {
      Entry& e = entries[i];
      if (e.pref_addr_tag != 0) {
        fmt::print("[HOLDING_TABLE_CONTENTS_ENTRY] pref_addr_tag: {:#x} base_addr_tag: {:#x} pc: {:#x}\n", e.pref_addr_tag, e.base_addr_tag, e.pc);
      }
    }
    assert(false);
  } else {
    for (std::size_t i = 0; i < capacity; ++i) {
      std::size_t idx = (eviction_idx + i) % capacity;
      Entry& e = entries[idx];
      if (e.pref_addr_tag != 0) {
        if (owner->DEBUG)
          fmt::print("[HOLDING_EVICT] Evicting oldest entry at idx {}: pref_addr_tag: {:#x} base_addr_tag: {:#x} pc: {:#x}\n", idx, e.pref_addr_tag,
                     e.base_addr_tag, e.pc);
        e = {pref_addr_tag, base_addr_tag, pc};
        eviction_idx = (idx + 1) % capacity;
        if (owner->DEBUG)
          fmt::print("[HOLDING_INSERT] pref_addr_tag: {:#x} base_addr_tag: {:#x} pc: {:#x} size: {}\n", pref_addr_tag, base_addr_tag, pc, count);
        return;
      }
    }
  }
}

std::optional<HoldingTable::Entry> HoldingTable::lookup(uint64_t pref_addr_tag)
{
  for (std::size_t i = 0; i < capacity; ++i) {
    Entry& e = entries[i];

    if (e.pref_addr_tag == pref_addr_tag && e.base_addr_tag != 0) {
      Entry result = e;
      count--;
      e = Entry{};
      if (owner->DEBUG)
        fmt::print("[HOLDING_REMOVE] pref_addr_tag: {:#x} base_addr_tag: {:#x} pc: {:#x} remaining: {}\n", result.pref_addr_tag, result.base_addr_tag, result.pc,
                   count);
      return result;
    }
  }
  if (owner->DEBUG)
    fmt::print("[HOLDING_MISS] pref_addr_tag: {:#x} not found\n", pref_addr_tag);
  return std::nullopt;
}

bool HoldingTable::test(uint64_t pref_addr_tag)
{
  for (std::size_t i = 0; i < capacity; ++i) {
    Entry& e = entries[i];
    if (e.pref_addr_tag == pref_addr_tag && e.base_addr_tag != 0) {
      return true;
    }
  }
  return false;
}

void HoldingTable::remove(uint64_t pref_addr_tag)
{
  for (std::size_t i = 0; i < capacity; ++i) {
    Entry& e = entries[i];
    if (e.pref_addr_tag == pref_addr_tag) {
      e = Entry{};
      count--;
      if (owner->DEBUG)
        fmt::print("[HOLDING_REMOVE] pref_addr_tag: {:#x} base_addr_tag: {:#x} pc: {:#x} remaining: {}\n", pref_addr_tag, e.base_addr_tag, e.pc, count);
      return;
    }
  }
}

/////////////////////////////////////////////////////////////
// Accuracy Table
/////////////////////////////////////////////////////////////

AccuracyTable::AccuracyTable(std::size_t size) : table_size(size), table(size, std::vector<uint16_t>(NUM_OFFSETS, 8)) {}

std::size_t AccuracyTable::getIndex(uint64_t pc) const { return (pc ^ (pc / table_size)) % table_size; }

int16_t AccuracyTable::lookup(uint64_t pc, std::size_t offset_idx) const
{
  std::size_t idx = getIndex(pc);
  if (offset_idx < static_cast<std::size_t>(NUM_OFFSETS)) {
    return static_cast<int16_t>(table[idx][offset_idx]);
  }
  return 0;
}

void AccuracyTable::increment(uint64_t pc, std::size_t offset_idx)
{
  std::size_t idx = getIndex(pc);
  if (offset_idx < static_cast<std::size_t>(NUM_OFFSETS)) {
    if (table[idx][offset_idx] < ACC_MAX) {
      uint16_t new_val = static_cast<uint16_t>(table[idx][offset_idx] + ACCURACY_INCREMENT);
      table[idx][offset_idx] = std::min<uint16_t>(ACC_MAX, new_val);
    }
  }
}

void AccuracyTable::decrement(uint64_t pc, std::size_t offset_idx)
{
  std::size_t idx = getIndex(pc);
  if (offset_idx < static_cast<std::size_t>(NUM_OFFSETS)) {
    if (table[idx][offset_idx] > ACC_MIN) {
      uint16_t new_val = (table[idx][offset_idx] > ACCURACY_DECREMENT) ? static_cast<uint16_t>(table[idx][offset_idx] - ACCURACY_DECREMENT) : ACC_MIN;
      table[idx][offset_idx] = std::max<uint16_t>(ACC_MIN, new_val);
    }
  }
}

void AccuracyTable::resetOffsetStats(std::size_t offset_idx)
{
  if (offset_idx >= static_cast<std::size_t>(NUM_OFFSETS))
    return;

  for (std::size_t i = 0; i < table_size; ++i) {
    table[i][offset_idx] = 8;
  }
}

/////////////////////////////////////////////////////////////
// caerus prefetcher
/////////////////////////////////////////////////////////////

std::size_t caerus::next_training_offset_idx()
{
  current_learning_offset_idx = (current_learning_offset_idx + 1) % learned_offsets.size();
  std::size_t wrap_count = 0;

  while (learned_offsets[current_learning_offset_idx] == 0) {
    current_learning_offset_idx = (current_learning_offset_idx + 1) % learned_offsets.size();
    wrap_count++;

    if (wrap_count > learned_offsets.size()) {
      current_learning_offset_idx = 0;
      break;
    }
  }

  return current_learning_offset_idx;
}

void caerus::resetScores()
{
  for (auto& it : offsetsList) {
    it.second = 0;
  }
}

void caerus::offsetLearning(uint64_t line_addr, uint8_t cache_hit)
{
  if (DEBUG)
    fmt::print("[OFFSET_LEARNING] line_addr: {:#x} cache_hit: {}\n", line_addr, cache_hit);

  // Skip learning if a learned offset already covers this address
  for (std::size_t i = 0; i < learned_offsets.size(); ++i) {
    if (i == current_learning_offset_idx)
      continue;

    if (learned_offsets[i] == 0)
      continue;

    uint64_t prev_pf_line_addr = line_addr - learned_offsets[i];

    if (rr_table.test(addr_tag(prev_pf_line_addr))) {
      if (accuracy_table.lookup(rr_table.lookup(addr_tag(prev_pf_line_addr)).pc, i) >= ACCURACY_THRESHOLD) {
        if (DEBUG)
          fmt::print("[OFFSET_LEARNING] skipping offset: {} (accurate overlap detected)\n", learned_offsets[i]);
        return;
      }
    }
  }

  std::vector<uint64_t> rr_offsets = rr_table.genAllOffsets(addr_tag(line_addr));

  for (auto offset : rr_offsets) {
    if (offset == 0) {
      if (DEBUG)
        fmt::print("[RR_TABLE_OFFSET_TRAINING] skipping offset: {} (zero)\n", offset);
      continue;
    }

    if (offset > 256) {
      if (DEBUG)
        fmt::print("[RR_TABLE_OFFSET_TRAINING] skipping offset: {} (too large)\n", offset);
      continue;
    }

    if (std::find(learned_offsets.begin(), learned_offsets.end(), offset) != learned_offsets.end()) {
      if (DEBUG)
        fmt::print("[RR_TABLE_OFFSET_TRAINING] skipping offset: {} (already learned)\n", offset);
      if (learned_offsets[current_learning_offset_idx] != offset)
        continue;
    }

    auto it = std::find_if(offsetsList.begin(), offsetsList.end(), [offset](const auto& entry) { return entry.first == static_cast<int16_t>(offset); });

    if (it != offsetsList.end()) {
      it->second += 1;

      if (DEBUG)
        fmt::print("[RR_TABLE_OFFSET_TRAINING] incrementing offset score of {}\n", it->first);

      if (it->second >= bestScore) {
        bestScore = it->second;
        phaseBestOffset = offset;
      }
    }
  }

  round++;

  if ((bestScore >= scoreMax) || (round >= roundMax)) {

    if (DEBUG)
      fmt::print("--> [LEARNING_PHASE_END] bestScore: {} round: {}\n", bestScore, round);

    if (bestScore >= scoreMax)
      score_max_counter++;
    if (round >= roundMax)
      round_max_counter++;

    if (bestScore > badScore) {
      if (phaseBestOffset != learned_offsets[current_learning_offset_idx])
        accuracy_table.resetOffsetStats(current_learning_offset_idx);

      learned_offsets[current_learning_offset_idx] = phaseBestOffset;

      current_learning_offset_idx = (current_learning_offset_idx + 1) % learned_offsets.size();

    } else {
      bad_score_counter++;

      if (DEBUG)
        fmt::print("[UNDER_BAD_SCORE] Current learning offset idx {}\n", current_learning_offset_idx);
      learned_offsets[current_learning_offset_idx] = 0;
      if (DEBUG)
        fmt::print("[CURRENT_LEARNING_OFFSET_IDX] {}\n", current_learning_offset_idx);
      if (DEBUG)
        fmt::print("\n");

      if (DEBUG)
        fmt::print("[UNDER_BAD_SCORE] offset is now: {}\n", learned_offsets[current_learning_offset_idx]);
      accuracy_table.resetOffsetStats(current_learning_offset_idx);
      current_learning_offset_idx = next_training_offset_idx();

      if (DEBUG)
        fmt::print("[UNDER_BAD_SCORE] Next learning offset idx {}\n", current_learning_offset_idx);
    }

    for (const auto& entry : offsetsList) {
      if (DEBUG)
        fmt::print("[OFFSETS_LIST_ENTRY] offset: {} score: {}\n", entry.first, entry.second);
    }
    if (DEBUG)
      fmt::print("\n");

    for (int i = 0; i < NUM_OFFSETS; ++i) {
      auto offset = learned_offsets[i];
      if (DEBUG)
        fmt::print("[LEARNED_OFFSET] offset{}: {}\n", i, offset);
    }
    if (DEBUG)
      fmt::print("[CURRENT_LEARNING_OFFSET_IDX] {}\n", current_learning_offset_idx);

    round = 0;
    bestScore = 0;
    phaseBestOffset = 0;
    resetScores();
  }
}

std::vector<uint64_t> caerus::calculateAccuratePrefetchAddrs(uint64_t full_addr, uint64_t line_addr, uint64_t pc)
{
  std::vector<uint64_t> pf_addrs;

  for (uint64_t cnt = 0, i = current_learning_offset_idx; cnt < learned_offsets.size(); ++cnt, i = (i + 1) % learned_offsets.size()) {

    auto offset = learned_offsets[i];
    if (DEBUG)
      fmt::print("[CALCULATE_ACCURATE_PF_ADDRS] offset: {}\n", offset);
    if (offset == 0) {
      continue;
    }

    if (accuracy_table.lookup(pc, i) < ACCURACY_THRESHOLD) {
      if (DEBUG)
        fmt::print("[CALCULATE_ACCURATE_PF_ADDRS] accuracy_table.lookup(pc, i) < ACCURACY_THRESHOLD\n");
      continue;
    }

    uint64_t pf_addr = full_addr + (offset << LOG2_BLOCK_SIZE);

    if (!ALLOW_CROSS_PAGE_PREFETCH) {
      if ((full_addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
        if (DEBUG)
          fmt::print("[CALCULATE_ACCURATE_PF_ADDRS] pf_addr: {:#x} crosses page boundary\n", pf_addr);
        continue;
      }
    }

    pf_addrs.push_back(pf_addr);
  }

  return pf_addrs;
}

std::vector<uint64_t> caerus::calculateAllPrefetchAddrs(uint64_t line_addr)
{
  std::vector<uint64_t> pf_addrs;

  for (auto offset : learned_offsets) {
    if (offset == 0) {
      pf_addrs.push_back(0);
      continue;
    }

    uint64_t pf_addr = (line_addr + offset) << LOG2_BLOCK_SIZE; // shift to get full addr bits

    if (!ALLOW_CROSS_PAGE_PREFETCH) {
      if (((line_addr << LOG2_BLOCK_SIZE) >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
        pf_addrs.push_back(0); // represent page crosses by 0
        continue;
      }
    }

    pf_addrs.push_back(pf_addr);
  }

  return pf_addrs;
}

void caerus::accuracy_train(uint64_t line_addr, uint64_t pc)
{
  if (pc == 0)
    return;

  std::vector<uint64_t> pf_addrs = calculateAllPrefetchAddrs(line_addr);

  for (int i = 0; i < NUM_OFFSETS; ++i) {
    if (i >= static_cast<int>(pf_addrs.size())) {
      break;
    }

    if (pf_addrs[i] == 0) {
      continue;
    }

    uint64_t pf_line_addr = pf_addrs[i] >> LOG2_BLOCK_SIZE;

    if (rr_table.test_partial(addr_tag(pf_line_addr))) {
      accuracy_table.increment(pc, i);
      increment_counter++;
    } else if (!rr_table.test(addr_tag(pf_line_addr))) {
      accuracy_table.decrement(pc, i);
      decrement_counter++;
    }
  }
}

void caerus::rr_insert_and_train_acc(uint64_t line_addr, uint64_t pc)
{
  RRTable::Entry evicted_entry = rr_table.insert(addr_tag(line_addr), pc);
  if (evicted_entry.addr_tag != 0) {
    accuracy_train(evicted_entry.addr_tag, evicted_entry.pc);
  }
}

void caerus::holding_insert(uint64_t pref_line_addr, uint64_t base_line_addr, uint64_t pc)
{
  if (!rr_table.test(addr_tag(pref_line_addr))) {
    holding_table.insert(addr_tag(pref_line_addr), addr_tag(base_line_addr), pc);
  }
}

void caerus::insertFill(uint64_t line_addr)
{
  if (DEBUG)
    fmt::print("[INSERT_FILL] line_addr: {:#x}\n", line_addr);
  auto result = holding_table.lookup(addr_tag(line_addr));
  if (result.has_value()) {
    rr_insert_and_train_acc(result->base_addr_tag, result->pc);
  }
}

uint64_t caerus::addr_tag(uint64_t line_addr) const
{
  uint64_t tagMask = (1ULL << ADDR_TAG_BITS) - 1;
  uint64_t PAGEBLOCKSIZE = (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE);
  uint64_t pageMask = (1ULL << PAGEBLOCKSIZE) - 1;
  uint64_t crunch = line_addr >> PAGEBLOCKSIZE;
  uint64_t crunchBits = ADDR_TAG_BITS - PAGEBLOCKSIZE;
  // Fold all higher bits down into the lower crunchBits
  for (uint64_t shift = crunchBits; shift < 64; shift *= 2)
    crunch ^= crunch >> shift;
  return ((line_addr & pageMask) | ((crunch & ((1ULL << crunchBits) - 1)) << PAGEBLOCKSIZE)) & tagMask;
}

void caerus::prefetcher_initialize()
{
  if (!champsim::msl::is_power_of_2(RR_SIZE)) {
    throw std::invalid_argument{"Number of RR entries is not power of 2\n"};
  }
  if (!champsim::msl::is_power_of_2(BLOCK_SIZE)) {
    throw std::invalid_argument{"Cache line size is not power of 2\n"};
  }
  if (!champsim::msl::is_power_of_2(HOLDING_TABLE_SIZE)) {
    throw std::invalid_argument{"Holding table size is not power of 2\n"};
  }

  if (NUMERIC_OFFSETS_ENABLE) {
    for (int i = 1; i < 256; i++) {
      offsetsList.push_back(OffsetListEntry(i, 0));
    }
    if (NEGATIVE_OFFSETS_ENABLE) {
      for (int i = 1; i < 256; i++) {
        offsetsList.push_back(OffsetListEntry(-i, 0));
      }
    }
  } else {
    const int factors[] = {2, 3, 5};
    unsigned int i = 0;
    int64_t offset_i = 1;

    while (i < OFFSET_LIST_SIZE) {
      int64_t offset = offset_i;

      for (int n : factors) {
        while ((offset % n) == 0) {
          offset /= n;
        }
      }

      if (offset == 1) {
        offsetsList.push_back(OffsetListEntry(offset_i, 0));
        i++;

        if (NEGATIVE_OFFSETS_ENABLE) {
          offsetsList.push_back(OffsetListEntry(-offset_i, 0));
          i++;
        }
      }

      offset_i++;
    }
  }

  for (const auto& entry : offsetsList) {
    if (DEBUG)
      fmt::print("[OFFSET_LIST_ENTRY] offset: {} metadata: {}\n", entry.first, entry.second);
  }
  if (DEBUG)
    fmt::print("\n");

  std::cout << "CAERUS Prefetcher Initialise" << std::endl;
  std::cout << "CAERUS DEBUG MODE: " << DEBUG << std::endl;

  std::cout << "+----------------------------+------------------+" << std::endl;
  std::cout << "|        Parameter           |      Value       |" << std::endl;
  std::cout << "+----------------------------+------------------+" << std::endl;
  std::cout << "| CAERUS SCORE_MAX           | " << std::setw(16) << SCORE_MAX << " |" << std::endl;
  std::cout << "| CAERUS ROUND_MAX           | " << std::setw(16) << ROUND_MAX << " |" << std::endl;
  std::cout << "| CAERUS BAD_SCORE           | " << std::setw(16) << BAD_SCORE << " |" << std::endl;
  std::cout << "| CAERUS OFFSET_LIST_SIZE    | " << std::setw(16) << OFFSET_LIST_SIZE << " |" << std::endl;
  std::cout << "| CAERUS NUM_OFFSETS         | " << std::setw(16) << NUM_OFFSETS << " |" << std::endl;
  std::cout << "| CAERUS ACCURACY_THRESHOLD  | " << std::setw(16) << ACCURACY_THRESHOLD << " |" << std::endl;
  std::cout << "| CAERUS ACCURACY_INCREMENT  | " << std::setw(16) << ACCURACY_INCREMENT << " |" << std::endl;
  std::cout << "| CAERUS ACCURACY_DECREMENT  | " << std::setw(16) << ACCURACY_DECREMENT << " |" << std::endl;
  std::cout << "| CAERUS RR_SIZE             | " << std::setw(16) << RR_SIZE << " |" << std::endl;
  std::cout << "| CAERUS RR_ENTRY_SIZE       | " << std::setw(16) << RR_ENTRY_SIZE << " |" << std::endl;
  std::cout << "| CAERUS HOLDING_TABLE_SIZE  | " << std::setw(16) << HOLDING_TABLE_SIZE << " |" << std::endl;
  std::cout << "| CAERUS HOLDING_ASSERT_FULL | " << std::setw(16) << HOLDING_TABLE_ASSERT_ON_FULL << " |" << std::endl;
  std::cout << "| CAERUS ACCURACY_TABLE_SIZE | " << std::setw(16) << ACCURACY_TABLE_SIZE << " |" << std::endl;
  std::cout << "| CAERUS ALLOW_CROSS_PAGE    | " << std::setw(16) << ALLOW_CROSS_PAGE_PREFETCH << " |" << std::endl;
  std::cout << "+----------------------------+------------------+" << std::endl;
}

uint32_t caerus::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                          uint32_t metadata_in)
{
  if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
    return metadata_in; // Not a load or prefetch
  }

  // Skip duplicate demand misses: if this miss hits an in-flight non-prefetch
  // MSHR entry, it's a duplicate and shouldn't notify the prefetcher.
  // Only MSHR hits on prefetch entries (late prefetches) are real misses.
  if (!cache_hit) {
    auto block_addr = champsim::block_number{addr};
    auto mshr_entry = std::find_if(intern_->MSHR.begin(), intern_->MSHR.end(),
                                   [block_addr](const auto& entry) { return champsim::block_number{entry.address} == block_addr; });
    if (mshr_entry != intern_->MSHR.end() && mshr_entry->type != access_type::PREFETCH) {
      return metadata_in;
    }
  }

  uint64_t raw_addr = addr.to<uint64_t>();
  uint64_t raw_ip = ip.to<uint64_t>();
  uint64_t line_addr = raw_addr >> LOG2_BLOCK_SIZE;
  uint64_t a_tag = addr_tag(line_addr);

  if (holding_table.test(a_tag) && cache_hit && useful_prefetch) {
    if (DEBUG)
      fmt::print("[PREFETCHER_CACHE_OPERATE] IN HOLDING: {:#x}\n", a_tag);
    holding_table.remove(a_tag);
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {

    if (DEBUG)
      fmt::print("\n==============================================\n");
    if (DEBUG)
      fmt::print("[PREFETCHER_CACHE_OPERATE] addr: {:#x} line_addr: {:#x} ip: {:#x} cache_hit: {} useful_prefetch: {}\n", raw_addr, line_addr, raw_ip,
                 cache_hit, useful_prefetch);

    auto pf_addrs = calculateAccuratePrefetchAddrs(raw_addr, line_addr, raw_ip);
    if (DEBUG)
      fmt::print("[CALCULATE_ACCURATE_PF_ADDRS] pf_addrs size: {}\n", pf_addrs.size());
    for (auto pf_addr : pf_addrs) {
      if (DEBUG)
        fmt::print("[CALCULATE_ACCURATE_PF_ADDRS] pf_addr: {:#x}\n", pf_addr);
    }

    bool added_to_holding = false;

    if (!rr_table.isSamePage(addr_tag(line_addr))) {
      if (DEBUG)
        fmt::print("[RR_TABLE_INSERT] First element of page, inserting into RR table {:#x}\n", line_addr);
      rr_insert_and_train_acc(line_addr, raw_ip);
      subtimely_rr_insertion_counter++;
      added_to_holding = true;
    }

    if (pf_addrs.size() > 0) {

      for (auto pf_addr : pf_addrs) {

        if (pf_addr == 0)
          continue;
        uint64_t pf_line_addr = pf_addr >> LOG2_BLOCK_SIZE;

        if (holding_table.test(addr_tag(pf_line_addr))) {
          if (DEBUG)
            fmt::print("[PREFETCH_TEST] Skipping due to duplicate prefetch address: {:#x}\n", pf_addr);
          continue;
        }

        if (intern_->get_pq_occupancy().back() < intern_->get_pq_size().back()
            && intern_->get_pq_occupancy().back() + intern_->get_mshr_occupancy() < intern_->get_mshr_size() - 1) {
          below_threshold_counter++;
        }
        general_pf_counter++;

        champsim::address pf_addr_strong{pf_addr};
        bool issued = prefetch_line(pf_addr_strong, true, 0x1);
        if (issued) {
          if (DEBUG)
            fmt::print("[PREFETCH] pf_addr: {:#x} with offset: {}\n", pf_addr, pf_line_addr - line_addr);

          if (!added_to_holding) {
            if (DEBUG)
              fmt::print("[HOLDING_TABLE_INSERT] Inserting into holding table due to prefetch {:#x}\n", pf_addr);
            holding_insert(pf_line_addr, line_addr, raw_ip);
            added_to_holding = true;
          }
        }
      }
    } else if (cache_hit && useful_prefetch) {
      rr_insert_and_train_acc(line_addr, raw_ip);
    } else {
      if (DEBUG)
        fmt::print("Observed cache miss for line_addr: {:#x}\n", line_addr);
      if (!added_to_holding) {
        if (DEBUG)
          fmt::print("[HOLDING_TABLE_INSERT] Inserting into holding table due to cache miss {:#x}\n", line_addr);
        holding_insert(line_addr, line_addr, raw_ip);
        added_to_holding = true;
      }
    }

    offsetLearning(line_addr, cache_hit);
    if (DEBUG)
      fmt::print("==============================================\n\n");
  }

  return metadata_in;
}

uint32_t caerus::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  uint64_t line_addr = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  uint64_t evicted_line_addr = evicted_addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;

  if (rr_table.test(addr_tag(evicted_line_addr))) {
    if (DEBUG)
      fmt::print("[PREFETCHER_CACHE_FILL] IN RR: {:#x}\n", evicted_line_addr);
    evict_in_rr_counter++;
  }

  insertFill(line_addr);

  return metadata_in;
}

void caerus::prefetcher_final_stats()
{
  std::cout << "================================================" << std::endl;
  std::cout << "CAERUS Round Max Counter: " << round_max_counter << std::endl;
  std::cout << "CAERUS Score Max Counter: " << score_max_counter << std::endl;
  std::cout << "CAERUS Bad Score Counter: " << bad_score_counter << std::endl;
  std::cout << "CAERUS Increment Counter: " << increment_counter << std::endl;
  std::cout << "CAERUS Decrement Counter: " << decrement_counter << std::endl;
  std::cout << "CAERUS Holding Table Eviction Counter: " << holding_table_eviction_counter << std::endl;
  std::cout << "CAERUS Holding Max Occupancy: " << holding_max_occupancy << std::endl;
  std::cout << "CAERUS Subtimely RR Insertion Counter: " << subtimely_rr_insertion_counter << std::endl;
  std::cout << "CAERUS Evict in RR Counter: " << evict_in_rr_counter << std::endl;
  const double below_threshold_pct =
      general_pf_counter ? (static_cast<double>(below_threshold_counter) * 100.0) / static_cast<double>(general_pf_counter) : 0.0;
  std::cout << "CAERUS Below Threshold Counter: " << below_threshold_pct << "%" << std::endl;
}
