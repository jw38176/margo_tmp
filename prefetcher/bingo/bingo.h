/* Bingo [https://mshakerinava.github.io/papers/bingo-hpca19.pdf]
 *
 * Ported to the modern ChampSim module interface used by Margo.
 *
 * The original algorithm (the filter / accumulation / pattern-history tables and
 * the prefetch buffer) is preserved inside the `bingo_space` namespace. The two
 * structural changes relative to the legacy implementation are:
 *   1. The set-associative-cache framework and the small helper utilities that
 *      Bingo used to pull in from `custom_util.h` are now self-contained here, so
 *      the prefetcher has no external dependency outside of ChampSim itself.
 *   2. The prefetch buffer no longer talks to a CACHE directly. Instead it is
 *      parameterised on a capacity predicate and an issue callback, so it can
 *      either issue prefetches directly (standalone use) or collect candidates
 *      for Margo (see margo_wrapper.{h,cc}).
 */

#ifndef PREFETCHER_BINGO_H
#define PREFETCHER_BINGO_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "champsim.h"
#include "modules.h"

namespace bingo_space
{

using std::cerr;
using std::endl;
using std::hex;
using std::ostringstream;
using std::setfill;
using std::setw;
using std::string;
using std::unordered_map;
using std::vector;

/*=== Bingo configuration (storage budget identical to the original) ===*/

inline constexpr int REGION_SIZE = 4 * 1024;
inline constexpr int MIN_ADDR_WIDTH = 6;
inline constexpr int MAX_ADDR_WIDTH = 16;
inline constexpr int PC_WIDTH = 16;
inline constexpr int PHT_SIZE = 4096;
inline constexpr int PHT_WAY = 16;
inline constexpr int FT_SIZE = 64;
inline constexpr int FT_WAY = 8;
inline constexpr int AT_SIZE = 64;
inline constexpr int AT_WAY = 8;
inline constexpr int PB_SIZE = 32;
inline constexpr int PB_WAY = 8;

inline constexpr double L2C_THRESH = 0.2;

inline constexpr int FILL_L1 = 1;
inline constexpr int FILL_L2 = 2;
inline constexpr int FILL_LLC = 3;

/* PC+Address matches are filled into this level. */
inline constexpr int PC_ADDRESS_FILL_LEVEL = FILL_L2;

inline constexpr int DEBUG_LEVEL = 0;

template <class T>
inline T square(T x)
{
  return x * x;
}

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
inline vector<T> my_rotate(const vector<T>& x, int n)
{
  vector<T> y;
  int len = static_cast<int>(x.size());
  if (len == 0)
    return y;
  n = n % len;
  for (int i = 0; i < len; i += 1)
    y.push_back(x[static_cast<size_t>((i - n + len) % len)]);
  return y;
}

template <class T>
inline string pattern_to_string(const vector<T>& pattern)
{
  ostringstream oss;
  for (unsigned i = 0; i < pattern.size(); i += 1)
    oss << int(pattern[i]) << " ";
  return oss.str();
}

/**
 * A class for printing beautiful data tables.
 * It's useful for logging the information contained in tabular structures.
 */
class Table
{
public:
  Table(int table_width, int table_height) : width(table_width), height(table_height), cells(table_height, vector<string>(table_width)) {}

  void set_row(int row, const vector<string>& data, int start_col = 0)
  {
    for (unsigned col = start_col; col < this->width; col += 1)
      this->set_cell(row, static_cast<int>(col), data[col]);
  }

  void set_col(int col, const vector<string>& data, int start_row = 0)
  {
    for (unsigned row = start_row; row < this->height; row += 1)
      this->set_cell(static_cast<int>(row), col, data[row]);
  }

  void set_cell(int row, int col, string data) { this->cells[static_cast<size_t>(row)][static_cast<size_t>(col)] = data; }

  void set_cell(int row, int col, double data)
  {
    ostringstream oss;
    oss << setw(11) << std::fixed << std::setprecision(8) << data;
    this->set_cell(row, col, oss.str());
  }

  void set_cell(int row, int col, int64_t data)
  {
    ostringstream oss;
    oss << setw(11) << std::left << data;
    this->set_cell(row, col, oss.str());
  }

  void set_cell(int row, int col, int data) { this->set_cell(row, col, (int64_t)data); }

  void set_cell(int row, int col, uint64_t data)
  {
    ostringstream oss;
    oss << "0x" << setfill('0') << setw(16) << hex << data;
    this->set_cell(row, col, oss.str());
  }

  /**
   * @return The entire table as a string
   */
  string to_string()
  {
    vector<int> widths;
    for (unsigned i = 0; i < this->width; i += 1)
    {
      int max_width = 0;
      for (unsigned j = 0; j < this->height; j += 1)
        max_width = std::max(max_width, (int)this->cells[j][i].size());
      widths.push_back(max_width + 2);
    }
    string out;
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

  string data_row(int row, const vector<int>& widths)
  {
    string out;
    for (unsigned i = 0; i < this->width; i += 1)
    {
      string data = this->cells[static_cast<size_t>(row)][i];
      data.resize(static_cast<size_t>(widths[i] - 2), ' ');
      out += " | " + data;
    }
    out += " |\n";
    return out;
  }

  static string top_line(const vector<int>& widths) { return Table::line(widths, "┌", "┬", "┐"); }

  static string mid_line(const vector<int>& widths) { return Table::line(widths, "├", "┼", "┤"); }

  static string bot_line(const vector<int>& widths) { return Table::line(widths, "└", "┴", "┘"); }

  static string line(const vector<int>& widths, string left, string mid, string right)
  {
    string out = " " + left;
    for (unsigned i = 0; i < widths.size(); i += 1)
    {
      int w = widths[i];
      for (int j = 0; j < w; j += 1)
        out += "─";
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
  vector<vector<string>> cells;
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
      : size(cache_size), num_ways(cache_num_ways), num_sets(size / num_ways), entries(static_cast<size_t>(num_sets), vector<Entry>(static_cast<size_t>(num_ways))),
        cams(static_cast<size_t>(num_sets)), debug_level(cache_debug_level)
  {
    for (int i = 0; i < num_sets; i += 1)
      for (int j = 0; j < num_ways; j += 1)
        entries[static_cast<size_t>(i)][static_cast<size_t>(j)].valid = false;
    /* calculate `index_len` (number of bits required to store the index) */
    for (int max_index = num_sets - 1; max_index > 0; max_index >>= 1)
      this->index_len += 1;
  }

  virtual ~SetAssociativeCache() = default;

  /**
   * Invalidates the entry corresponding to the given key.
   * @return A pointer to the invalidated entry
   */
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

  /**
   * @return The old state of the entry that was updated
   */
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
    vector<Entry>& set = this->entries[index];
    int victim_way = -1;
    for (int i = 0; i < this->num_ways; i += 1)
      if (!set[static_cast<size_t>(i)].valid)
      {
        victim_way = i;
        break;
      }
    if (victim_way == -1)
    {
      victim_way = this->select_victim(index);
    }
    Entry& victim = set[static_cast<size_t>(victim_way)];
    Entry old_entry = victim;
    victim = {key, index, tag, true, data};
    auto& cam = cams[index];
    if (old_entry.valid)
    {
      cam.erase(old_entry.tag);
    }
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

  /**
   * Creates a table with the given headers and populates the rows by calling `write_data` on all
   * valid entries contained in the cache.
   * @return The constructed table as a string
   */
  string log(vector<string> headers)
  {
    vector<Entry> valid_entries = this->get_valid_entries();
    Table table(static_cast<int>(headers.size()), static_cast<int>(valid_entries.size()) + 1);
    table.set_row(0, headers);
    for (unsigned i = 0; i < valid_entries.size(); i += 1)
      this->write_data(valid_entries[i], table, static_cast<int>(i) + 1);
    return table.to_string();
  }

  int get_index_len() { return this->index_len; }

  void set_debug_level(int level) { this->debug_level = level; }

protected:
  /* should be overriden in children */
  virtual void write_data(Entry& entry, Table& table, int row) {}

  /**
   * @return The way of the selected victim
   */
  virtual int select_victim(uint64_t index)
  {
    /* random eviction policy if not overriden */
    return rand() % this->num_ways;
  }

  vector<Entry> get_valid_entries()
  {
    vector<Entry> valid_entries;
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
  vector<vector<Entry>> entries;
  vector<unordered_map<uint64_t, int>> cams;
  int debug_level = 0;
};

template <class T>
class LRUSetAssociativeCache : public SetAssociativeCache<T>
{
  typedef SetAssociativeCache<T> Super;

public:
  LRUSetAssociativeCache(int size, int num_ways, int debug_level = 0)
      : Super(size, num_ways, debug_level), lru(static_cast<size_t>(this->num_sets), vector<uint64_t>(static_cast<size_t>(num_ways)))
  {
  }

  void set_mru(uint64_t key) { *this->get_lru(key) = this->t++; }

  void set_lru(uint64_t key) { *this->get_lru(key) = 0; }

  /* Generic replacement-policy hooks (kept for parity with the original framework). */
  void rp_insert(uint64_t key) { set_mru(key); }

  void rp_promote(uint64_t key) { set_mru(key); }

protected:
  /* @override */
  int select_victim(uint64_t index)
  {
    vector<uint64_t>& lru_set = this->lru[index];
    return static_cast<int>(std::min_element(lru_set.begin(), lru_set.end()) - lru_set.begin());
  }

  uint64_t* get_lru(uint64_t key)
  {
    uint64_t index = key % static_cast<uint64_t>(this->num_sets);
    uint64_t tag = key / static_cast<uint64_t>(this->num_sets);
    int way = this->cams[index][tag];
    return &this->lru[index][static_cast<size_t>(way)];
  }

  vector<vector<uint64_t>> lru;
  uint64_t t = 1;
};

/*=== End Of Cache Framework ===*/

class FilterTableData
{
public:
  uint64_t pc;
  int offset;
};

class FilterTable : public LRUSetAssociativeCache<FilterTableData>
{
  typedef LRUSetAssociativeCache<FilterTableData> Super;

public:
  FilterTable(int ft_size, int ft_debug_level = 0, int ft_num_ways = 16) : Super(ft_size, ft_num_ways, ft_debug_level) {}

  Entry* find(uint64_t region_number)
  {
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (!entry)
      return nullptr;
    Super::set_mru(key);
    return entry;
  }

  void insert(uint64_t region_number, uint64_t pc, int offset)
  {
    uint64_t key = this->build_key(region_number);
    Super::insert(key, {pc, offset});
    Super::set_mru(key);
  }

  Entry* erase(uint64_t region_number)
  {
    uint64_t key = this->build_key(region_number);
    return Super::erase(key);
  }

  string log()
  {
    vector<string> headers({"Region", "PC", "Offset"});
    return Super::log(headers);
  }

private:
  /* @override */
  void write_data(Entry& entry, Table& table, int row)
  {
    uint64_t key = hash_index(entry.key, this->index_len);
    table.set_cell(row, 0, key);
    table.set_cell(row, 1, entry.data.pc);
    table.set_cell(row, 2, entry.data.offset);
  }

  uint64_t build_key(uint64_t region_number)
  {
    uint64_t key = region_number & ((1ULL << 37) - 1);
    return hash_index(key, this->index_len);
  }
};

class AccumulationTableData
{
public:
  uint64_t pc;
  int offset;
  vector<bool> pattern;
};

class AccumulationTable : public LRUSetAssociativeCache<AccumulationTableData>
{
  typedef LRUSetAssociativeCache<AccumulationTableData> Super;

public:
  AccumulationTable(int at_size, int at_pattern_len, int at_debug_level = 0, int at_num_ways = 16)
      : Super(at_size, at_num_ways, at_debug_level), pattern_len(at_pattern_len)
  {
  }

  /**
   * @return False if the tag wasn't found and true if the pattern bit was successfully set
   */
  bool set_pattern(uint64_t region_number, int offset)
  {
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (!entry)
      return false;
    entry->data.pattern[static_cast<size_t>(offset)] = true;
    Super::set_mru(key);
    return true;
  }

  /* NOTE: `region_number` is probably truncated since it comes from the filter table */
  Entry insert(uint64_t region_number, uint64_t pc, int offset)
  {
    uint64_t key = this->build_key(region_number);
    vector<bool> pattern(static_cast<size_t>(this->pattern_len), false);
    pattern[static_cast<size_t>(offset)] = true;
    Entry old_entry = Super::insert(key, {pc, offset, pattern});
    Super::set_mru(key);
    return old_entry;
  }

  Entry* erase(uint64_t region_number)
  {
    uint64_t key = this->build_key(region_number);
    return Super::erase(key);
  }

  string log()
  {
    vector<string> headers({"Region", "PC", "Offset", "Pattern"});
    return Super::log(headers);
  }

private:
  /* @override */
  void write_data(Entry& entry, Table& table, int row)
  {
    uint64_t key = hash_index(entry.key, this->index_len);
    table.set_cell(row, 0, key);
    table.set_cell(row, 1, entry.data.pc);
    table.set_cell(row, 2, entry.data.offset);
    table.set_cell(row, 3, pattern_to_string(entry.data.pattern));
  }

  uint64_t build_key(uint64_t region_number)
  {
    uint64_t key = region_number & ((1ULL << 37) - 1);
    return hash_index(key, this->index_len);
  }

  int pattern_len;
};

enum Event
{
  PC_ADDRESS = 0,
  PC_OFFSET = 1,
  MISS = 2
};

class PatternHistoryTableData
{
public:
  vector<bool> pattern;
};

class PatternHistoryTable : public LRUSetAssociativeCache<PatternHistoryTableData>
{
  typedef LRUSetAssociativeCache<PatternHistoryTableData> Super;

public:
  PatternHistoryTable(int pht_size, int pht_pattern_len, int pht_min_addr_width, int pht_max_addr_width, int pht_pc_width, int pht_debug_level = 0,
                      int pht_num_ways = 16)
      : Super(pht_size, pht_num_ways, pht_debug_level), pattern_len(pht_pattern_len), min_addr_width(pht_min_addr_width), max_addr_width(pht_max_addr_width),
        pc_width(pht_pc_width)
  {
  }

  /* NOTE: In BINGO, address is actually block number. */
  void insert(uint64_t pc, uint64_t address, vector<bool> pattern)
  {
    int offset = static_cast<int>(address % static_cast<uint64_t>(this->pattern_len));
    pattern = my_rotate(pattern, -offset);
    uint64_t key = this->build_key(pc, address);
    Super::insert(key, {pattern});
    Super::set_mru(key);
  }

  /**
   * First searches for a PC+Address match. If no match is found, returns all PC+Offset matches.
   * @return All un-rotated patterns if matches were found, returns an empty vector otherwise
   */
  vector<vector<bool>> find(uint64_t pc, uint64_t address)
  {
    uint64_t key = this->build_key(pc, address);
    uint64_t index = key % static_cast<uint64_t>(this->num_sets);
    uint64_t tag = key / static_cast<uint64_t>(this->num_sets);
    auto& set = this->entries[index];
    uint64_t min_tag_mask = (1ULL << (this->pc_width + this->min_addr_width - this->index_len)) - 1;
    uint64_t max_tag_mask = (1ULL << (this->pc_width + this->max_addr_width - this->index_len)) - 1;
    vector<vector<bool>> matches;
    this->last_event = MISS;
    for (int i = 0; i < this->num_ways; i += 1)
    {
      if (!set[static_cast<size_t>(i)].valid)
        continue;
      bool min_match = ((set[static_cast<size_t>(i)].tag & min_tag_mask) == (tag & min_tag_mask));
      bool max_match = ((set[static_cast<size_t>(i)].tag & max_tag_mask) == (tag & max_tag_mask));
      vector<bool>& cur_pattern = set[static_cast<size_t>(i)].data.pattern;
      if (max_match)
      {
        this->last_event = PC_ADDRESS;
        Super::set_mru(set[static_cast<size_t>(i)].key);
        matches.clear();
        matches.push_back(cur_pattern);
        break;
      }
      if (min_match)
      {
        this->last_event = PC_OFFSET;
        matches.push_back(cur_pattern);
      }
    }
    int offset = static_cast<int>(address % static_cast<uint64_t>(this->pattern_len));
    for (int i = 0; i < (int)matches.size(); i += 1)
      matches[static_cast<size_t>(i)] = my_rotate(matches[static_cast<size_t>(i)], +offset);
    return matches;
  }

  Event get_last_event() { return this->last_event; }

  string log()
  {
    vector<string> headers({"PC", "Offset", "Address", "Pattern"});
    return Super::log(headers);
  }

private:
  /* @override */
  void write_data(Entry& entry, Table& table, int row)
  {
    uint64_t base_key = entry.key >> (this->pc_width + this->min_addr_width);
    uint64_t index_key = entry.key & ((1ULL << (this->pc_width + this->min_addr_width)) - 1);
    index_key = hash_index(index_key, this->index_len); /* unhash */
    uint64_t key = (base_key << (this->pc_width + this->min_addr_width)) | index_key;

    /* extract PC, offset, and address */
    uint64_t offset = key & ((1ULL << this->min_addr_width) - 1);
    key >>= this->min_addr_width;
    uint64_t pc = key & ((1ULL << this->pc_width) - 1);
    key >>= this->pc_width;
    uint64_t address = (key << this->min_addr_width) + offset;

    table.set_cell(row, 0, pc);
    table.set_cell(row, 1, offset);
    table.set_cell(row, 2, address);
    table.set_cell(row, 3, pattern_to_string(entry.data.pattern));
  }

  uint64_t build_key(uint64_t pc, uint64_t address)
  {
    pc &= (1ULL << this->pc_width) - 1;            /* use `pc_width` bits from pc */
    address &= (1ULL << this->max_addr_width) - 1; /* use `addr_width` bits from address */
    uint64_t offset = address & ((1ULL << this->min_addr_width) - 1);
    uint64_t base = (address >> this->min_addr_width);
    /* key = base + hash_index( pc + offset )
     * The index must be computed from only PC+Offset to ensure that all entries with the same
     * PC+Offset end up in the same set */
    uint64_t index_key = hash_index((pc << this->min_addr_width) | offset, this->index_len);
    uint64_t key = (base << (this->pc_width + this->min_addr_width)) | index_key;
    return key;
  }

  int pattern_len;
  int min_addr_width, max_addr_width, pc_width;
  Event last_event = MISS;
};

class PrefetchBufferData
{
public:
  vector<int> pattern;
};

class PrefetchBuffer : public LRUSetAssociativeCache<PrefetchBufferData>
{
  typedef LRUSetAssociativeCache<PrefetchBufferData> Super;

public:
  PrefetchBuffer(int pb_size, int pb_pattern_len, int pb_debug_level = 0, int pb_num_ways = 16)
      : Super(pb_size, pb_num_ways, pb_debug_level), pattern_len(pb_pattern_len)
  {
  }

  void insert(uint64_t region_number, vector<int> pattern)
  {
    uint64_t key = this->build_key(region_number);
    Super::insert(key, {pattern});
    Super::rp_insert(key);
  }

  /**
   * Generates prefetch candidates for the most recent access.
   * @param has_capacity Predicate returning whether another prefetch may be issued right now
   * @param issue Callback that attempts to issue a prefetch; returns true if it was issued.
   *              It receives (pf_addr, fill_this_level, metadata, region_number, pf_offset).
   * @param defer_commit When true (candidate/Margo mode) the pattern is not consumed here; the
   *              caller is expected to call clear_offset() from its notify_result once a candidate is
   *              actually issued, and the entry is kept alive so un-issued offsets can be retried on a
   *              later access (mirroring the standalone behaviour under capacity pressure).
   */
  template <typename CapacityFn, typename IssueFn>
  int prefetch(uint64_t block_num, const CapacityFn& has_capacity, const IssueFn& issue, unsigned log2_block_size, bool defer_commit = false)
  {
    int region_offset = static_cast<int>(block_num % static_cast<uint64_t>(this->pattern_len));
    uint64_t region_number = block_num / static_cast<uint64_t>(this->pattern_len);
    uint64_t key = this->build_key(region_number);
    Entry* entry = Super::find(key);
    if (!entry)
      return 0;
    Super::set_mru(key);
    int pf_issued = 0;
    bool produced_any = false;
    vector<int>& pattern = entry->data.pattern;
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
            bool fill_this_level = (pattern[static_cast<size_t>(pf_offset)] == FILL_L2);
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
    // In immediate mode the region is fully consumed. In deferred mode only drop the entry once it
    // has nothing left to offer, so un-issued offsets remain available for a later access.
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

  string log()
  {
    vector<string> headers({"Region", "Pattern"});
    return Super::log(headers);
  }

private:
  /* @override */
  void write_data(Entry& entry, Table& table, int row)
  {
    uint64_t key = hash_index(entry.key, this->index_len);
    table.set_cell(row, 0, key);
    table.set_cell(row, 1, pattern_to_string(entry.data.pattern));
  }

  uint64_t build_key(uint64_t region_number) { return hash_index(region_number, this->index_len); }

  int pattern_len;
};

class Bingo
{
public:
  Bingo(int bingo_pattern_len, int min_addr_width, int max_addr_width, int pc_width, int filter_table_size, int filter_table_way, int accumulation_table_size,
        int accumulation_table_way, int pht_size, int pht_ways, int pb_size, int pb_way, int bingo_debug_level = 0)
      : pattern_len(bingo_pattern_len), filter_table(filter_table_size, bingo_debug_level, filter_table_way),
        accumulation_table(accumulation_table_size, bingo_pattern_len, bingo_debug_level, accumulation_table_way),
        pht(pht_size, bingo_pattern_len, min_addr_width, max_addr_width, pc_width, bingo_debug_level, pht_ways),
        pf_buffer(pb_size, bingo_pattern_len, bingo_debug_level, pb_way), debug_level(bingo_debug_level)
  {
  }

  void access(uint64_t block_number, uint64_t pc)
  {
    uint64_t region_number = block_number / static_cast<uint64_t>(this->pattern_len);
    int region_offset = static_cast<int>(block_number % static_cast<uint64_t>(this->pattern_len));
    bool success = this->accumulation_table.set_pattern(region_number, region_offset);
    if (success)
      return;
    FilterTable::Entry* entry = this->filter_table.find(region_number);
    if (!entry)
    {
      /* trigger access */
      this->filter_table.insert(region_number, pc, region_offset);
      vector<int> pattern = this->find_in_phts(pc, block_number);
      if (pattern.empty())
        return;

      this->pf_buffer.insert(region_number, pattern);
      return;
    }
    if (entry->data.offset != region_offset)
    {
      /* move from filter table to accumulation table */
      uint64_t ft_region_number = hash_index(entry->key, this->filter_table.get_index_len());
      AccumulationTable::Entry victim = this->accumulation_table.insert(ft_region_number, entry->data.pc, entry->data.offset);
      this->accumulation_table.set_pattern(ft_region_number, region_offset);
      this->filter_table.erase(ft_region_number);
      if (victim.valid)
      {
        /* move from accumulation table to pattern history table */
        this->insert_in_phts(victim);
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
    /* end of generation */
    uint64_t region_number = block_number / static_cast<uint64_t>(this->pattern_len);
    this->filter_table.erase(region_number);
    AccumulationTable::Entry* entry = this->accumulation_table.erase(region_number);
    if (entry)
    {
      /* move from accumulation table to pattern history table */
      this->insert_in_phts(*entry);
    }
  }

  void set_debug_level(int level) { this->debug_level = level; }

  void log()
  {
    cerr << "Filter table begin" << std::dec << endl;
    cerr << this->filter_table.log();
    cerr << "Filter table end" << endl;

    cerr << "Accumulation table begin" << std::dec << endl;
    cerr << this->accumulation_table.log();
    cerr << "Accumulation table end" << endl;

    cerr << "PHT table begin" << std::dec << endl;
    cerr << this->pht.log();
    cerr << "PHT table end" << endl;

    cerr << "Prefetch buffer begin" << std::dec << endl;
    cerr << this->pf_buffer.log();
    cerr << "Prefetch buffer end" << endl;
  }

private:
  vector<int> find_in_phts(uint64_t pc, uint64_t address)
  {
    if (this->debug_level >= 1)
      cerr << "[Bingo] find_in_phts(pc=" << pc << ", address=" << address << ")" << endl;
    vector<vector<bool>> matches = this->pht.find(pc, address);
    this->pht_access_cnt += 1;
    Event pht_last_event = this->pht.get_last_event();
    uint64_t region_number = address / static_cast<uint64_t>(this->pattern_len);
    if (pht_last_event != MISS)
      this->pht_events[region_number] = pht_last_event;
    vector<int> pattern;
    if (pht_last_event == PC_ADDRESS)
    {
      this->pht_pc_address_cnt += 1;
      pattern.resize(static_cast<size_t>(this->pattern_len), 0);
      for (int i = 0; i < this->pattern_len; i += 1)
        if (matches[0][static_cast<size_t>(i)])
          pattern[static_cast<size_t>(i)] = PC_ADDRESS_FILL_LEVEL;
    }
    else if (pht_last_event == PC_OFFSET)
    {
      this->pht_pc_offset_cnt += 1;
      pattern = this->vote(matches);
    }
    else if (pht_last_event == MISS)
    {
      this->pht_miss_cnt += 1;
    }
    /* stats */
    if (pht_last_event != MISS)
    {
      this->region_pref_cnt += 1;
      for (int i = 0; i < (int)pattern.size(); i += 1)
        if (pattern[static_cast<size_t>(i)] != 0)
          this->pref_level_cnt[pattern[static_cast<size_t>(i)]] += 1;
    }
    return pattern;
  }

  void insert_in_phts(const AccumulationTable::Entry& entry)
  {
    if (this->debug_level >= 1)
      cerr << "[Bingo] insert_in_phts(...)" << endl;
    uint64_t pc = entry.data.pc;
    uint64_t region_number = hash_index(entry.key, this->accumulation_table.get_index_len());
    uint64_t address = region_number * static_cast<uint64_t>(this->pattern_len) + static_cast<uint64_t>(entry.data.offset);
    const vector<bool>& pattern = entry.data.pattern;
    this->pht.insert(pc, address, pattern);
  }

  vector<int> vote(const vector<vector<bool>>& x)
  {
    int n = static_cast<int>(x.size());
    if (n == 0)
      return vector<int>();
    /* stats */
    this->vote_cnt += 1;
    this->voter_sum += static_cast<uint64_t>(n);
    this->voter_sqr_sum += square(static_cast<uint64_t>(n));
    /* ===== */
    bool pf_flag = false;
    vector<int> res(static_cast<size_t>(this->pattern_len), 0);
    for (int i = 0; i < this->pattern_len; i += 1)
    {
      int cnt = 0;
      for (int j = 0; j < n; j += 1)
        if (x[static_cast<size_t>(j)][static_cast<size_t>(i)])
          cnt += 1;
      double p = 1.0 * cnt / n;
      if (p >= L2C_THRESH)
        res[static_cast<size_t>(i)] = FILL_L2;
      else
        res[static_cast<size_t>(i)] = 0;
      if (res[static_cast<size_t>(i)] != 0)
        pf_flag = true;
    }
    if (!pf_flag)
      return vector<int>();
    return res;
  }

  int pattern_len;
  FilterTable filter_table;
  AccumulationTable accumulation_table;
  PatternHistoryTable pht;
  PrefetchBuffer pf_buffer;
  int debug_level = 0;

  /* stats */
  unordered_map<uint64_t, Event> pht_events;

  uint64_t pht_access_cnt = 0;
  uint64_t pht_pc_address_cnt = 0;
  uint64_t pht_pc_offset_cnt = 0;
  uint64_t pht_miss_cnt = 0;

  unordered_map<int, uint64_t> pref_level_cnt;
  uint64_t region_pref_cnt = 0;

  uint64_t vote_cnt = 0;
  uint64_t voter_sum = 0;
  uint64_t voter_sqr_sum = 0;
};

} // namespace bingo_space

/**
 * ChampSim prefetcher module wrapping the Bingo algorithm. It exposes both the
 * standard module hooks (so it can be used as a normal ChampSim prefetcher) and
 * a `prefetch_candidates` method used by the Margo sub-prefetcher framework.
 */
class bingo : public champsim::modules::prefetcher
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
  std::unique_ptr<bingo_space::Bingo> pf;

  bool _collect_candidates = false;
  std::vector<Candidate> _candidates;
  champsim::address _trigger_ip{};

  void operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type);
};

#endif
