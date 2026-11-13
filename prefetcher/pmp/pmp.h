/* PMP (Pattern-based Multi-level Prefetcher)
 *
 * Ported to the modern ChampSim module interface used by Margo.
 *
 * The original algorithm (filter / accumulation tables, the offset- and
 * PC-indexed confidence pattern tables, and the prefetch buffer) is preserved
 * inside the `pmp_space` namespace. The two structural changes relative to the
 * legacy implementation are:
 *   1. The set-associative-cache framework and the small helper utilities that
 *      PMP used to pull in from `custom_util.h` are now self-contained here (in
 *      `pmp_space::custom_util`), so the prefetcher has no external dependency
 *      outside of ChampSim itself.
 *   2. The prefetch buffer no longer talks to a CACHE directly. Instead it is
 *      parameterised on a capacity predicate and an issue callback, so it can
 *      either issue prefetches directly (standalone use) or collect candidates
 *      for Margo (see margo_wrapper.{h,cc}). The per-offset fill-level
 *      decision (FILL_L2 -> fill this level, otherwise -> next level) matches
 *      the legacy `prefetch_line(..., pattern==FILL_L2_PMP, ...)` behaviour.
 */

#ifndef PREFETCHER_PMP_H
#define PREFETCHER_PMP_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "champsim.h"
#include "modules.h"

namespace pmp_space
{

/*=== PMP configuration (storage budget identical to the original) ===*/

inline constexpr int BOTTOM_BITS = 6;
inline constexpr int PC_BITS = 8;
inline constexpr int BACKOFF_TIMES = 1;

inline constexpr int IN_REGION_BITS = 12;
inline constexpr int OFFSET_BITS = IN_REGION_BITS - BOTTOM_BITS;
inline constexpr int OFFSET_MASK = (1 << OFFSET_BITS) - 1;

inline constexpr int START_CONF = 0;
inline constexpr int PATTERN_DEGRADE_LEVEL = 2;

inline constexpr int FILL_L1_PMP = 1;
inline constexpr int FILL_L2_PMP = 2;
inline constexpr int FILL_LLC_PMP = 3;

// The original code sizes the region using ChampSim's BLOCK_SIZE. IN_REGION_BITS
// = 12 (4 KiB region) with 64 B blocks gives 64 blocks per region.
inline constexpr int PATTERN_LEN = (1 << IN_REGION_BITS) / 64;

// Kept for parity with the legacy final-stats output. They are never incremented
// by the algorithm itself, so they always read zero (as in the original).
inline int filter_by_ppt = 0;
inline int prefetch_to_l1 = 0;
inline int prefetch_to_l2 = 0;

// Legacy offset helpers. With BOTTOM_BITS == LOG2_BLOCK_SIZE the coarse and fine
// offsets are identical, but the shift is kept so the maths matches the original.
#define __fine_offset(addr) ((addr) & OFFSET_MASK)
#define __coarse_offset(fine_offset) ((fine_offset) >> (6 - BOTTOM_BITS))

// Saturating increment used by the confidence pattern tables. Mirrors the
// original custom_util::ADD helper.
#define ADD(x, cap) ((x) = std::min<int>((x) + 1, (cap)))

namespace custom_util
{

/*=== Helper utilities (formerly custom_util.h) ===*/

/**
 * A very simple and efficient hash function that:
 * 1) Splits key into blocks of length `index_len` bits and computes the XOR of all blocks.
 * 2) Replaces the least significant block of key with the computed block.
 * Applying this hash function twice with the same `index_len` acts as the identity function.
 */
inline uint64_t hash_index(uint64_t key, int index_len)
{
  if (index_len == 0)
    return key;
  for (uint64_t tag = (key >> index_len); tag > 0; tag >>= index_len)
    key ^= tag & ((1ULL << index_len) - 1);
  return key;
}

template <class T>
inline std::vector<T> my_rotate(const std::vector<T>& x, int n)
{
  std::vector<T> y;
  int len = static_cast<int>(x.size());
  if (len == 0)
    return y;
  n = n % len;
  for (int i = 0; i < len; i += 1)
    y.push_back(x[static_cast<size_t>((i - n + len) % len)]);
  return y;
}

template <class T>
inline std::string pattern_to_string(const std::vector<T>& pattern)
{
  std::ostringstream oss;
  for (unsigned i = 0; i < pattern.size(); i += 1)
    oss << int(pattern[i]) << " ";
  return oss.str();
}

inline int count_bits(uint64_t x)
{
  int cnt = 0;
  for (; x; x &= (x - 1))
    cnt += 1;
  return cnt;
}

inline uint64_t pattern_to_int(const std::vector<bool>& pattern)
{
  uint64_t res = 0;
  for (size_t i = 0; i < pattern.size() && i < 64; i += 1)
    if (pattern[i])
      res |= (1ULL << i);
  return res;
}

/* Convert a boolean pattern into an integer (0/1) confidence pattern. */
inline std::vector<int> pattern_convert(const std::vector<bool>& pattern)
{
  std::vector<int> res(pattern.size(), 0);
  for (size_t i = 0; i < pattern.size(); i += 1)
    res[i] = pattern[i] ? 1 : 0;
  return res;
}

/* Reduce a pattern by OR-ing together each group of `level` consecutive blocks. */
inline std::vector<bool> pattern_degrade(const std::vector<bool>& pattern, int level)
{
  std::vector<bool> res(pattern.size() / static_cast<size_t>(level), false);
  for (size_t i = 0; i < pattern.size(); i += 1)
    if (pattern[i])
      res[i / static_cast<size_t>(level)] = true;
  return res;
}

/**
 * A class for printing beautiful data tables.
 * It's useful for logging the information contained in tabular structures.
 */
class Table
{
public:
  Table(int table_width, int table_height) : width(table_width), height(table_height), cells(table_height, std::vector<std::string>(table_width)) {}

  void set_row(int row, const std::vector<std::string>& data, int start_col = 0)
  {
    for (unsigned col = static_cast<unsigned>(start_col); col < this->width; col += 1)
      this->set_cell(row, static_cast<int>(col), data[col]);
  }

  void set_cell(int row, int col, std::string data) { this->cells[static_cast<size_t>(row)][static_cast<size_t>(col)] = data; }

  void set_cell(int row, int col, double data)
  {
    std::ostringstream oss;
    oss << std::setw(11) << std::fixed << std::setprecision(8) << data;
    this->set_cell(row, col, oss.str());
  }

  void set_cell(int row, int col, int64_t data)
  {
    std::ostringstream oss;
    oss << std::setw(11) << std::left << data;
    this->set_cell(row, col, oss.str());
  }

  void set_cell(int row, int col, int data) { this->set_cell(row, col, (int64_t)data); }

  void set_cell(int row, int col, uint64_t data)
  {
    std::ostringstream oss;
    oss << "0x" << std::setfill('0') << std::setw(16) << std::hex << data;
    this->set_cell(row, col, oss.str());
  }

  std::string to_string()
  {
    std::vector<int> widths;
    for (unsigned i = 0; i < this->width; i += 1)
    {
      int max_width = 0;
      for (unsigned j = 0; j < this->height; j += 1)
        max_width = std::max(max_width, (int)this->cells[j][i].size());
      widths.push_back(max_width + 2);
    }
    std::string out;
    out += Table::top_line(widths);
    out += this->data_row(0, widths);
    for (unsigned i = 1; i < this->height; i += 1)
    {
      out += Table::mid_line(widths);
      out += this->data_row(static_cast<int>(i), widths);
    }
    out += Table::bot_line(widths);
    return out;
  }

  std::string data_row(int row, const std::vector<int>& widths)
  {
    std::string out;
    for (unsigned i = 0; i < this->width; i += 1)
    {
      std::string data = this->cells[static_cast<size_t>(row)][i];
      data.resize(static_cast<size_t>(widths[i] - 2), ' ');
      out += " | " + data;
    }
    out += " |\n";
    return out;
  }

  static std::string top_line(const std::vector<int>& widths) { return Table::line(widths, "+", "+", "+"); }
  static std::string mid_line(const std::vector<int>& widths) { return Table::line(widths, "+", "+", "+"); }
  static std::string bot_line(const std::vector<int>& widths) { return Table::line(widths, "+", "+", "+"); }

  static std::string line(const std::vector<int>& widths, std::string left, std::string mid, std::string right)
  {
    std::string out = " " + left;
    for (unsigned i = 0; i < widths.size(); i += 1)
    {
      int w = widths[i];
      for (int j = 0; j < w; j += 1)
        out += "-";
      if (i != widths.size() - 1)
        out += mid;
      else
        out += right;
    }
    return out + "\n";
  }

private:
  unsigned width;
  unsigned height;
  std::vector<std::vector<std::string>> cells;
};

/*=== Cache Framework ===*/

template <class T>
class SetAssociativeCache
{
public:
  class Entry
  {
  public:
    uint64_t key;
    uint64_t index;
    uint64_t tag;
    bool valid;
    T data;
  };

  SetAssociativeCache(int cache_size, int cache_num_ways, int cache_debug_level = 0)
      : size(cache_size), num_ways(cache_num_ways), num_sets(size / num_ways),
        entries(static_cast<size_t>(num_sets), std::vector<Entry>(static_cast<size_t>(num_ways))), cams(static_cast<size_t>(num_sets)),
        debug_level(cache_debug_level)
  {
    for (int i = 0; i < num_sets; i += 1)
      for (int j = 0; j < num_ways; j += 1)
        entries[static_cast<size_t>(i)][static_cast<size_t>(j)].valid = false;
    for (int max_index = num_sets - 1; max_index > 0; max_index >>= 1)
      this->index_len += 1;
  }

  virtual ~SetAssociativeCache() = default;

  Entry* erase(uint64_t key)
  {
    Entry* entry = this->find(key);
    uint64_t index = key % static_cast<uint64_t>(this->num_sets);
    uint64_t tag = key / static_cast<uint64_t>(this->num_sets);
    auto& cam = cams[index];
    cam.erase(tag);
    if (entry)
      entry->valid = false;
    return entry;
  }

  Entry insert(uint64_t key, const T& data)
  {
    Entry* entry = this->find(key);
    if (entry != nullptr)
    {
      Entry old_entry = *entry;
      entry->data = data;
      return old_entry;
    }
    uint64_t index = key % static_cast<uint64_t>(this->num_sets);
    uint64_t tag = key / static_cast<uint64_t>(this->num_sets);
    std::vector<Entry>& set = this->entries[index];
    int victim_way = -1;
    for (int i = 0; i < this->num_ways; i += 1)
      if (!set[static_cast<size_t>(i)].valid)
      {
        victim_way = i;
        break;
      }
    if (victim_way == -1)
      victim_way = this->select_victim(index);
    Entry& victim = set[static_cast<size_t>(victim_way)];
    Entry old_entry = victim;
    victim = {key, index, tag, true, data};
    auto& cam = cams[index];
    if (old_entry.valid)
      cam.erase(old_entry.tag);
    cam[tag] = victim_way;
    return old_entry;
  }

  Entry* find(uint64_t key)
  {
    uint64_t index = key % static_cast<uint64_t>(this->num_sets);
    uint64_t tag = key / static_cast<uint64_t>(this->num_sets);
    auto& cam = cams[index];
    if (cam.find(tag) == cam.end())
      return nullptr;
    int way = cam[tag];
    Entry& entry = this->entries[index][static_cast<size_t>(way)];
    return &entry;
  }

  std::string log(std::vector<std::string> headers)
  {
    std::vector<Entry> valid_entries = this->get_valid_entries();
    Table table(static_cast<int>(headers.size()), static_cast<int>(valid_entries.size()) + 1);
    table.set_row(0, headers);
    for (unsigned i = 0; i < valid_entries.size(); i += 1)
      this->write_data(valid_entries[i], table, static_cast<int>(i) + 1);
    return table.to_string();
  }

  int get_index_len() { return this->index_len; }

  void set_debug_level(int level) { this->debug_level = level; }

protected:
  virtual void write_data(Entry& /*entry*/, Table& /*table*/, int /*row*/) {}

  virtual int select_victim(uint64_t /*index*/)
  {
    /* random eviction policy if not overriden */
    return rand() % this->num_ways;
  }

  std::vector<Entry> get_valid_entries()
  {
    std::vector<Entry> valid_entries;
    for (int i = 0; i < num_sets; i += 1)
      for (int j = 0; j < num_ways; j += 1)
        if (entries[static_cast<size_t>(i)][static_cast<size_t>(j)].valid)
          valid_entries.push_back(entries[static_cast<size_t>(i)][static_cast<size_t>(j)]);
    return valid_entries;
  }

  int size;
  int num_ways;
  int num_sets;
  int index_len = 0; /* in bits */
  std::vector<std::vector<Entry>> entries;
  std::vector<std::unordered_map<uint64_t, int>> cams;
  int debug_level = 0;
};

template <class T>
class LRUSetAssociativeCache : public SetAssociativeCache<T>
{
  typedef SetAssociativeCache<T> Super;

public:
  LRUSetAssociativeCache(int size, int num_ways, int debug_level = 0)
      : Super(size, num_ways, debug_level), lru(static_cast<size_t>(this->num_sets), std::vector<uint64_t>(static_cast<size_t>(num_ways)))
  {
  }

  void set_mru(uint64_t key) { *this->get_lru(key) = this->t++; }
  void set_lru(uint64_t key) { *this->get_lru(key) = 0; }

  /* Generic replacement-policy hooks (kept for parity with the original framework). */
  void rp_insert(uint64_t key) { set_mru(key); }
  void rp_promote(uint64_t key) { set_mru(key); }

protected:
  int select_victim(uint64_t index) override
  {
    std::vector<uint64_t>& lru_set = this->lru[index];
    return static_cast<int>(std::min_element(lru_set.begin(), lru_set.end()) - lru_set.begin());
  }

  uint64_t* get_lru(uint64_t key)
  {
    uint64_t index = key % static_cast<uint64_t>(this->num_sets);
    uint64_t tag = key / static_cast<uint64_t>(this->num_sets);
    int way = this->cams[index][tag];
    return &this->lru[index][static_cast<size_t>(way)];
  }

  std::vector<std::vector<uint64_t>> lru;
  uint64_t t = 1;
};

/*=== End Of Cache Framework ===*/

} // namespace custom_util

#define FT_CACHE_TYPE custom_util::LRUSetAssociativeCache
#define AT_CACHE_TYPE custom_util::LRUSetAssociativeCache
#define PS_CACHE_TYPE custom_util::LRUSetAssociativeCache

// SMS Filter Table Data <Tag, PC/Offset>
class FilterTableData
{
public:
  int offset;
  uint64_t pc;
};

// SMS Filter Table
class FilterTable : public FT_CACHE_TYPE<FilterTableData>
{
  typedef FT_CACHE_TYPE<FilterTableData> Super;

public:
  FilterTable(int size, int debug_level = 0, int num_ways = 16) : Super(size, num_ways)
  {
    if (this->debug_level >= 1)
      std::cerr << "FilterTable::FilterTable(size=" << size << ", debug_level=" << debug_level << ", num_ways=" << num_ways << ")" << std::dec << std::endl;
  }

  Entry* find(uint64_t region_number)
  {
    if (this->debug_level >= 2)
      std::cerr << "FilterTable::find(region_number=0x" << std::hex << region_number << ")" << std::dec << std::endl;
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (!entry)
    {
      if (this->debug_level >= 2)
        std::cerr << "[FilterTable::find] Miss!" << std::dec << std::endl;
      return nullptr;
    }
    if (this->debug_level >= 2)
      std::cerr << "[FilterTable::find] Hit!" << std::dec << std::endl;
    Super::rp_promote(key);
    return entry;
  }

  void insert(uint64_t region_number, int offset, uint64_t pc)
  {
    if (this->debug_level >= 2)
      std::cerr << "FilterTable::insert(region_number=0x" << std::hex << region_number << ", offset=" << std::dec << offset << ")" << std::dec << std::endl;
    uint64_t key = this->build_key(region_number);
    Super::insert(key, {offset, pc});
    Super::rp_insert(key);
  }

  Entry* erase(uint64_t region_number)
  {
    uint64_t key = this->build_key(region_number);
    return Super::erase(key);
  }

  std::string log()
  {
    std::vector<std::string> headers({"Region", "Offset"});
    return Super::log(headers);
  }

private:
  void write_data(Entry& entry, custom_util::Table& table, int row) override
  {
    uint64_t key = custom_util::hash_index(entry.key, this->index_len);
    table.set_cell(row, 0, key);
    table.set_cell(row, 1, entry.data.offset);
  }

  uint64_t build_key(uint64_t region_number)
  {
    uint64_t key = region_number & ((1ULL << 37) - 1);
    return custom_util::hash_index(key, this->index_len);
  }
};

class AccumulationTableData
{
public:
  int offset;
  int second_offset;
  uint64_t pc;
  std::vector<bool> pattern;
};

class AccumulationTable : public AT_CACHE_TYPE<AccumulationTableData>
{
  typedef AT_CACHE_TYPE<AccumulationTableData> Super;

public:
  AccumulationTable(int size, int pattern_len, int debug_level = 0, int num_ways = 16) : Super(size, num_ways), pattern_len(pattern_len)
  {
    if (this->debug_level >= 1)
      std::cerr << "AccumulationTable::AccumulationTable(size=" << size << ", pattern_len=" << pattern_len << ", debug_level=" << debug_level
                << ", num_ways=" << num_ways << ")" << std::dec << std::endl;
  }

  bool set_pattern(uint64_t region_number, int offset)
  {
    if (this->debug_level >= 2)
      std::cerr << "AccumulationTable::set_pattern(region_number=0x" << std::hex << region_number << ", offset=" << std::dec << offset << ")" << std::dec
                << std::endl;
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (!entry)
    {
      if (this->debug_level >= 2)
        std::cerr << "[AccumulationTable::set_pattern] Not found!" << std::dec << std::endl;
      return false;
    }
    entry->data.pattern[static_cast<size_t>(offset)] = true;
    Super::rp_promote(key);
    if (this->debug_level >= 2)
      std::cerr << "[AccumulationTable::set_pattern] OK!" << std::dec << std::endl;
    return true;
  }

  Entry insert(uint64_t region_number, uint64_t pc, int offset, int second_offset)
  {
    if (this->debug_level >= 2)
      std::cerr << "AccumulationTable::insert(region_number=0x" << std::hex << region_number << ", offset=" << std::dec << offset << std::dec << std::endl;
    uint64_t key = this->build_key(region_number);
    std::vector<bool> pattern(static_cast<size_t>(this->pattern_len), false);
    pattern[static_cast<size_t>(__coarse_offset(offset))] = true;
    Entry old_entry = Super::insert(key, {offset, second_offset, pc, pattern});
    Super::rp_insert(key);
    return old_entry;
  }

  Entry* erase(uint64_t region_number)
  {
    uint64_t key = this->build_key(region_number);
    return Super::erase(key);
  }

  std::string log()
  {
    std::vector<std::string> headers({"Region", "Offset", "Second", "Pattern"});
    return Super::log(headers);
  }

private:
  void write_data(Entry& entry, custom_util::Table& table, int row) override
  {
    uint64_t key = custom_util::hash_index(entry.key, this->index_len);
    table.set_cell(row, 0, key);
    table.set_cell(row, 1, entry.data.offset);
    table.set_cell(row, 2, entry.data.second_offset);
    table.set_cell(row, 3, custom_util::pattern_to_string(entry.data.pattern));
  }

  uint64_t build_key(uint64_t region_number)
  {
    uint64_t key = region_number & ((1ULL << 37) - 1);
    return custom_util::hash_index(key, this->index_len);
  }

  int pattern_len;
};

class OffsetPatternTableData
{
public:
  std::vector<int> pattern;
  int second_offset;
};

class OffsetPatternTable : public custom_util::LRUSetAssociativeCache<OffsetPatternTableData>
{
  typedef custom_util::LRUSetAssociativeCache<OffsetPatternTableData> Super;

public:
  OffsetPatternTable(int size, int pattern_len, int tag_size, int num_ways = 16, int max_conf = 16, int debug_level = 0, int cpu = 0)
      : Super(size, num_ways, debug_level), pattern_len(pattern_len), tag_size(tag_size), cpu(cpu), max_conf(max_conf)
  {
    if (this->debug_level >= 1)
      std::cerr << "OffsetPatternTable::OffsetPatternTable(size=" << size << ", pattern_len=" << pattern_len << ", tag_size=" << tag_size
                << ", debug_level=" << debug_level << ", num_ways=" << num_ways << ")" << std::dec << std::endl;
  }

  void insert(uint64_t address, uint64_t pc, std::vector<bool> pattern, bool is_degrade, int second_offset)
  {
    (void)pc;
    if (this->debug_level >= 2)
      std::cerr << "OffsetPatternTable::insert(" << std::hex << "address=0x" << address << ", pattern=" << custom_util::pattern_to_string(pattern) << ")"
                << std::dec << std::endl;
    int offset = __coarse_offset(__fine_offset(address));
    offset = is_degrade ? offset / PATTERN_DEGRADE_LEVEL : offset;
    pattern = custom_util::my_rotate(pattern, -offset);
    uint64_t key = this->build_key(address, pc);
    Entry* entry = Super::find(key);
    assert(pattern[0]);
    if (entry)
    {
      int max_value = 0;
      auto& stored_pattern = entry->data.pattern;
      for (int i = 0; i < this->pattern_len; i++)
      {
        pattern[static_cast<size_t>(i)] ? ADD(stored_pattern[static_cast<size_t>(i)], max_conf) : 0;
        if (i > 0 && max_value < stored_pattern[static_cast<size_t>(i)])
          max_value = stored_pattern[static_cast<size_t>(i)];
      }

      if (entry->data.pattern[0] == max_conf)
      {
        if (max_value < (1 << BACKOFF_TIMES))
          entry->data.pattern[0] = max_value;
        else
          for (auto& e : stored_pattern)
            e >>= BACKOFF_TIMES;
      }
      Super::rp_promote(key);
    }
    else
    {
      Super::insert(key, OffsetPatternTableData{custom_util::pattern_convert(pattern), second_offset});
      Super::rp_insert(key);
    }
  }

  std::vector<OffsetPatternTableData> find(uint64_t pc, uint64_t block_number)
  {
    if (this->debug_level >= 2)
      std::cerr << "OffsetPatternTable::find(pc=0x" << std::hex << pc << ", address=0x" << block_number << ")" << std::dec << std::endl;
    uint64_t key = this->build_key(block_number, pc);
    Entry* entry = Super::find(key);
    std::vector<OffsetPatternTableData> matches;
    if (entry)
    {
      auto& cur_pattern = entry->data;
      matches.push_back(cur_pattern);
    }
    return matches;
  }

  std::string log()
  {
    std::vector<std::string> headers({"Key", "Second", "Pattern"});
    return Super::log(headers);
  }

private:
  void write_data(Entry& entry, custom_util::Table& table, int row) override
  {
    table.set_cell(row, 0, entry.key);
    table.set_cell(row, 1, entry.data.second_offset);
    table.set_cell(row, 2, custom_util::pattern_to_string(entry.data.pattern));
  }

  virtual uint64_t build_key(uint64_t address, uint64_t pc)
  {
    (void)pc;
    uint64_t offset = __fine_offset(address);
    uint64_t key = offset & ((1 << this->tag_size) - 1);
    return key;
  }

protected:
  const int pattern_len;
  const int tag_size, cpu;
  const int max_conf;
};

class PCPatternTable : public OffsetPatternTable
{
public:
  PCPatternTable(int size, int pattern_len, int tag_size, int num_ways = 16, int max_conf = 32, int debug_level = 0, int cpu = 0)
      : OffsetPatternTable(size, pattern_len, tag_size, num_ways, max_conf, debug_level, cpu)
  {
  }

private:
  uint64_t build_key(uint64_t address, uint64_t pc) override
  {
    (void)address;
    return custom_util::hash_index(pc, this->index_len) & ((1 << this->tag_size) - 1);
  }
};

class PrefetchBufferData
{
public:
  std::vector<int> pattern;
};

class PrefetchBuffer : public PS_CACHE_TYPE<PrefetchBufferData>
{
  typedef PS_CACHE_TYPE<PrefetchBufferData> Super;

public:
  PrefetchBuffer(int size, int pattern_len, int debug_level = 0, int num_ways = 8) : Super(size, num_ways), pattern_len(pattern_len)
  {
    if (this->debug_level >= 1)
      std::cerr << "PrefetchBuffer::PrefetchBuffer(size=" << size << ", pattern_len=" << pattern_len << ", debug_level=" << debug_level
                << ", num_ways=" << num_ways << ")" << std::dec << std::endl;
  }

  void insert(uint64_t region_number, std::vector<int> pattern)
  {
    if (this->debug_level >= 2)
      std::cerr << "PrefetchBuffer::insert(region_number=0x" << std::hex << region_number << ", pattern=" << custom_util::pattern_to_string(pattern) << ")"
                << std::dec << std::endl;
    uint64_t key = this->build_key(region_number);
    Super::insert(key, {pattern});
    Super::rp_insert(key);
  }

  /**
   * Generates prefetch candidates for the most recent access.
   * @param has_capacity Predicate returning whether another prefetch may be issued right now.
   * @param issue Callback that attempts to issue a prefetch; returns true if it was issued.
   *              It receives (pf_addr, fill_this_level, metadata, region_number, pf_offset).
   * @param defer_commit When true (candidate/Margo mode) the pattern is not consumed here; the
   *              caller is expected to call clear_offset() from its notify_result once a candidate is
   *              actually issued, and the entry is kept alive so un-issued offsets can be retried on a
   *              later access (mirroring the standalone behaviour under capacity pressure).
   */
  template <typename CapacityFn, typename IssueFn>
  int prefetch(uint64_t block_address, const CapacityFn& has_capacity, const IssueFn& issue, unsigned log2_block_size, bool defer_commit = false)
  {
    int region_offset = __coarse_offset(__fine_offset(block_address));
    uint64_t region_number = block_address >> OFFSET_BITS;
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (!entry)
    {
      if (this->debug_level >= 2)
        std::cerr << "[PrefetchBuffer::prefetch] No entry found." << std::dec << std::endl;
      return 0;
    }
    Super::rp_promote(key);
    int pf_issued = 0;
    bool produced_any = false;
    std::vector<int>& pattern = entry->data.pattern;
    pattern[static_cast<size_t>(region_offset)] = 0;
    for (int d = 1; d < this->pattern_len; d += 1)
    {
      for (int sgn = +1; sgn >= -1; sgn -= 2)
      {
        int pf_offset = region_offset + sgn * d;
        if (0 <= pf_offset && pf_offset < this->pattern_len && pattern[static_cast<size_t>(pf_offset)] > 0)
        {
          uint64_t pf_address = (region_number * static_cast<uint64_t>(this->pattern_len) + static_cast<uint64_t>(pf_offset)) << log2_block_size;
          if (has_capacity())
          {
            // Legacy behaviour: FILL_L2_PMP entries fill this level, everything else goes deeper.
            bool fill_this_level = (pattern[static_cast<size_t>(pf_offset)] == FILL_L2_PMP);
            if (issue(pf_address, fill_this_level, 0u, region_number, pf_offset))
            {
              pf_issued += 1;
              produced_any = true;
              if (!defer_commit)
                pattern[static_cast<size_t>(pf_offset)] = 0;
            }
          }
          else
          {
            return pf_issued;
          }
        }
      }
    }
    if (!defer_commit || !produced_any)
      Super::erase(key);
    return pf_issued;
  }

  /* Deferred-mode commit: mark a single offset as prefetched once it has actually been issued. */
  void clear_offset(uint64_t region_number, int pf_offset)
  {
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (entry && pf_offset >= 0 && pf_offset < this->pattern_len)
      entry->data.pattern[static_cast<size_t>(pf_offset)] = 0;
  }

  std::string log()
  {
    std::vector<std::string> headers({"Region", "Pattern"});
    return Super::log(headers);
  }

private:
  void write_data(Entry& entry, custom_util::Table& table, int row) override
  {
    uint64_t key = custom_util::hash_index(entry.key, this->index_len);
    table.set_cell(row, 0, key);
    table.set_cell(row, 1, custom_util::pattern_to_string(entry.data.pattern));
  }

  uint64_t build_key(uint64_t region_number)
  {
    uint64_t key = region_number;
    return custom_util::hash_index(key, this->index_len);
  }

  int pattern_len;
};

class PMP
{
public:
  PMP(int pattern_len, int offset_width, int opt_size, int opt_max_conf, int opt_ways, int pc_width, int ppt_size, int ppt_max_conf, int ppt_ways,
      int filter_table_size, int ft_way, int accumulation_table_size, int at_way, int pf_buffer_size, int pf_buffer_way, int debug_level = 0, int cpu = 0)
      : pattern_len(pattern_len), opt(opt_size, pattern_len, offset_width, opt_ways, opt_max_conf, debug_level, cpu),
        ppt(ppt_size, pattern_len / PATTERN_DEGRADE_LEVEL, pc_width, ppt_ways, ppt_max_conf, debug_level, cpu),
        filter_table(filter_table_size, debug_level, ft_way), accumulation_table(accumulation_table_size, pattern_len, debug_level, at_way),
        pf_buffer(pf_buffer_size, pattern_len, debug_level, pf_buffer_way), debug_level(debug_level), cpu(cpu)
  {
    if (this->debug_level >= 1)
      std::cerr << " PMP:: PMP(pattern_len=" << pattern_len << ", filter_table_size=" << filter_table_size
                << ", accumulation_table_size=" << accumulation_table_size << ", pf_buffer_size=" << pf_buffer_size << ", debug_level=" << debug_level << ")"
                << std::endl;
  }

  void access(uint64_t block_number, uint64_t pc)
  {
    if (this->debug_level >= 2)
      std::cerr << "[ PMP] access(block_number=0x" << std::hex << block_number << ", pc=0x" << pc << ")" << std::dec << std::endl;

    uint64_t region_number = block_number >> OFFSET_BITS;
    int region_offset = __fine_offset(block_number);
    bool success = this->accumulation_table.set_pattern(region_number, __coarse_offset(region_offset));
    if (success)
      return;
    FilterTable::Entry* entry = this->filter_table.find(region_number);
    if (!entry)
    {
      this->filter_table.insert(region_number, region_offset, pc);
      std::vector<int> pattern = this->find_in_opt(pc, block_number);
      if (pattern.empty())
        return;

      this->pf_buffer.insert(region_number, pattern);
      return;
    }
    else if (entry->data.offset != region_offset)
    {
      uint64_t ft_region_number = custom_util::hash_index(entry->key, this->filter_table.get_index_len());
      AccumulationTable::Entry victim = this->accumulation_table.insert(ft_region_number, entry->data.pc, entry->data.offset, region_offset);
      this->accumulation_table.set_pattern(ft_region_number, __coarse_offset(region_offset));
      this->filter_table.erase(ft_region_number);
      if (victim.valid)
      {
        invalid_by_max++;
        this->insert_in_opt(victim, false);
      }
    }
  }

  template <typename CapacityFn, typename IssueFn>
  int prefetch(uint64_t block_number, const CapacityFn& has_capacity, const IssueFn& issue, unsigned log2_block_size, bool defer_commit = false)
  {
    return this->pf_buffer.prefetch(block_number, has_capacity, issue, log2_block_size, defer_commit);
  }

  void clear_offset(uint64_t region_number, int pf_offset) { this->pf_buffer.clear_offset(region_number, pf_offset); }

  void eviction(uint64_t block_number)
  {
    if (this->debug_level >= 2)
      std::cerr << "[ PMP] eviction(block_number=" << block_number << ")" << std::dec << std::endl;
    uint64_t region_number = block_number / static_cast<uint64_t>(this->pattern_len);
    this->filter_table.erase(region_number);
    AccumulationTable::Entry* entry = this->accumulation_table.erase(region_number);
    if (entry)
    {
      invalid_by_eviction++;
      this->insert_in_opt(*entry, true);
    }
  }

  void set_debug_level(int level)
  {
    this->filter_table.set_debug_level(level);
    this->accumulation_table.set_debug_level(level);
    this->opt.set_debug_level(level);
    this->ppt.set_debug_level(level);
    this->debug_level = level;
  }

  void log()
  {
    std::cerr << "Filter table begin" << std::dec << std::endl;
    std::cerr << this->filter_table.log();
    std::cerr << "Filter table end" << std::endl;

    std::cerr << "Accumulation table begin" << std::dec << std::endl;
    std::cerr << this->accumulation_table.log();
    std::cerr << "Accumulation table end" << std::endl;

    std::cerr << "Offset pattern table begin" << std::dec << std::endl;
    std::cerr << this->opt.log();
    std::cerr << "Offset pattern table end" << std::endl;

    std::cerr << "PC pattern table begin" << std::dec << std::endl;
    std::cerr << this->ppt.log();
    std::cerr << "PC pattern table end" << std::endl;

    std::cerr << "Prefetch buffer begin" << std::dec << std::endl;
    std::cerr << this->pf_buffer.log();
    std::cerr << "Prefetch buffer end" << std::endl;
  }

  int invalid_by_eviction = 0;
  int invalid_by_max = 0;
  uint64_t num_pht_updates = 0;
  uint64_t num_end_of_generation_updates = 0;

private:
  std::vector<int> find_in_opt(uint64_t pc, uint64_t block_number)
  {
    if (this->debug_level >= 2)
      std::cerr << "[ PMP] find_in_opt(pc=0x" << std::hex << pc << ", address=0x" << block_number << ")" << std::dec << std::endl;
    std::vector<OffsetPatternTableData> matches = this->opt.find(pc, block_number);
    std::vector<OffsetPatternTableData> matches_pc = this->ppt.find(pc, block_number);
    std::vector<int> pattern;
    std::vector<int> pattern_pc;
    std::vector<int> result_pattern(static_cast<size_t>(this->pattern_len), 0);
    if (!matches.empty())
    {
      pattern = this->vote(matches);
      pattern_pc = this->vote(matches_pc, true);
      // The PC pattern table is consulted for parity with the original, but the
      // active variant simply mirrors the offset table's decision.
      if (pattern_pc.empty())
      {
        for (int i = 0; i < this->pattern_len; i++)
          result_pattern[static_cast<size_t>(i)] = pattern[static_cast<size_t>(i)];
      }
      else
      {
        for (int i = 0; i < this->pattern_len; i++)
          result_pattern[static_cast<size_t>(i)] = pattern[static_cast<size_t>(i)];
      }
    }

    int offset = __coarse_offset(__fine_offset(block_number));
    result_pattern = custom_util::my_rotate(result_pattern, +offset);
    return result_pattern;
  }

  void insert_in_opt(const AccumulationTable::Entry& entry, bool is_end_of_generation)
  {
    num_pht_updates++;
    if (is_end_of_generation)
      num_end_of_generation_updates++;

    uint64_t region_number = custom_util::hash_index(entry.key, this->accumulation_table.get_index_len());
    uint64_t address = (region_number << OFFSET_BITS) + static_cast<uint64_t>(entry.data.offset);
    if (this->debug_level >= 2)
      std::cerr << "[ PMP] insert_in_opt(" << std::hex << " address=0x" << address << ")" << std::dec << std::endl;
    const std::vector<bool>& pattern = entry.data.pattern;
    if (custom_util::count_bits(custom_util::pattern_to_int(pattern)) != 1)
    {
      this->opt.insert(address, entry.data.pc, pattern, false, entry.data.second_offset);
      this->ppt.insert(address, entry.data.pc, custom_util::pattern_degrade(pattern, PATTERN_DEGRADE_LEVEL), true, entry.data.second_offset);
    }
  }

  std::vector<int> vote(const std::vector<OffsetPatternTableData>& x, bool is_pc_opt = false)
  {
    if (this->debug_level >= 2)
      std::cerr << " PMP::vote(...)" << std::endl;
    int n = static_cast<int>(x.size());
    if (n == 0)
    {
      if (this->debug_level >= 2)
        std::cerr << "[ PMP::vote] There are no voters." << std::endl;
      return std::vector<int>();
    }

    if (this->debug_level >= 2)
    {
      std::cerr << "[ PMP::vote] Taking a vote among:" << std::endl;
      for (int i = 0; i < n; i += 1)
        std::cerr << "<" << std::setw(3) << i + 1 << "> " << custom_util::pattern_to_string(x[static_cast<size_t>(i)].pattern) << std::endl;
    }
    int len = is_pc_opt ? this->pattern_len / PATTERN_DEGRADE_LEVEL : this->pattern_len;
    std::vector<int> res(static_cast<size_t>(len), 0);

    for (int i = 0; i < len; i += 1)
    {
      int cnt = 0;
      for (int j = 0; j < n; j += 1)
        cnt += x[static_cast<size_t>(j)].pattern[static_cast<size_t>(i)];
      double p = 1.0 * cnt / x[0].pattern[0];
      if (p > 1)
      {
        std::cout << "cnt:" << cnt << ",total:" << x[0].pattern[0] << std::endl;
        assert(p <= 1);
      }

      if (x[0].pattern[0] <= START_CONF)
        break;

      if (is_pc_opt)
      {
        if (p >= PC_L1D_THRESH)
          res[static_cast<size_t>(i)] = FILL_L1_PMP;
        else if (p >= PC_L2C_THRESH)
          res[static_cast<size_t>(i)] = FILL_L2_PMP;
        else if (p >= PC_LLC_THRESH)
          res[static_cast<size_t>(i)] = FILL_LLC_PMP;
        else
          res[static_cast<size_t>(i)] = 0;
      }
      else
      {
        if (p >= L1D_THRESH)
          res[static_cast<size_t>(i)] = FILL_L1_PMP;
        else if (p >= L2C_THRESH)
          res[static_cast<size_t>(i)] = FILL_L2_PMP;
        else if (p >= LLC_THRESH)
          res[static_cast<size_t>(i)] = FILL_LLC_PMP;
        else
          res[static_cast<size_t>(i)] = 0;
      }
    }
    if (this->debug_level >= 2)
      std::cerr << "<res> " << custom_util::pattern_to_string(res) << std::endl;

    return res;
  }

  const double L1D_THRESH = 2.0; /* off */
  const double L2C_THRESH = 0.50;
  const double LLC_THRESH = 0.150;

  const double PC_L1D_THRESH = 2.0; /* off */
  const double PC_L2C_THRESH = 0.50;
  const double PC_LLC_THRESH = 0.150;

  /*======================*/

  int pattern_len;
  OffsetPatternTable opt;
  PCPatternTable ppt;
  FilterTable filter_table;
  AccumulationTable accumulation_table;
  PrefetchBuffer pf_buffer;
  int debug_level = 0;
  int cpu;
};

/*=== PMP tuning parameters (identical storage budget to the original) ===*/

inline constexpr int FT_SIZE = 64;
inline constexpr int FT_WAY = 8;
inline constexpr int AT_SIZE = 64;
inline constexpr int AT_WAY = 8;
inline constexpr int OPT_WAYS = 1;
inline constexpr int OPT_SIZE = (1 << OFFSET_BITS) * OPT_WAYS;
inline constexpr int OFFSET_MAX_CONF = 32;
inline constexpr int PPT_WAYS = 1;
inline constexpr int PPT_SIZE = (1 << PC_BITS) * PPT_WAYS;
inline constexpr int PC_MAX_CONF = 32;
inline constexpr int PF_BUFFER_SIZE = 32;
inline constexpr int PF_BUFFER_WAY = 8;
inline constexpr int DEBUG_LEVEL = 0;

} // namespace pmp_space

/**
 * ChampSim prefetcher module wrapping the PMP algorithm. It exposes both the
 * standard module hooks (so it can be used as a normal ChampSim prefetcher) and
 * a `prefetch_candidates` method used by the Margo sub-prefetcher framework.
 */
class pmp : public champsim::modules::prefetcher
{
public:
  struct Candidate
  {
    champsim::address address{};
    champsim::address trigger_ip{};
    bool fill_this_level = true;
    uint32_t metadata = 0;
    std::function<void(bool issued, bool filtered_out)> notify_result{};
  };

  using prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();

  std::vector<Candidate> prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                             uint32_t metadata_in);

private:
  std::unique_ptr<pmp_space::PMP> pf;

  bool _collect_candidates = false;
  std::vector<Candidate> _candidates;
  champsim::address _trigger_ip{};

  void operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type);
};

#endif
