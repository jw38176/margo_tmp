#ifndef GAZE_H
#define GAZE_H

/*
 * Gaze into the Pattern: Characterizing Spatial Patterns with Internal Temporal Correlations for Hardware Prefetching
 *
 * To appear in 31st IEEE International Symposium on High-Performance Computer Architecture (HPCA 2025),
 * 3/1/2025-3/5/2025, Las Vegas, NV, USA
 *
 * @Authors: Zixiao Chen, Chentao Wu, Yunfei Gu, Ranhao Jia, Jie Li, and Minyi Guo
 * @Manteiners: Zixiao Chen
 * @Email: chen_zx@sjtu.edu.cn
 * @Date: 12/02/2024
 */

#include "access_type.h"
#include "address.h"
#include "cache.h"
#include "modules.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <deque>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace custom_util
{

class Table
{
public:
    template <typename T>
    void set_cell(int, int, const T&) {}
};

uint64_t hash_index(uint64_t key, int index_len);
uint64_t my_hash_index(uint64_t key, int shift, int index_len);

template <typename T>
std::string pattern_to_string(const std::vector<T>& pattern)
{
    std::ostringstream stream;
    for (const auto& value : pattern)
        stream << value;
    return stream.str();
}

template <>
inline std::string pattern_to_string<bool>(const std::vector<bool>& pattern)
{
    std::ostringstream stream;
    for (bool value : pattern)
        stream << (value ? '1' : '0');
    return stream.str();
}

template <typename Data>
class LRUSetAssociativeCache
{
public:
    struct Entry
    {
        bool valid = false;
        uint64_t key = 0;
        Data data{};
        uint64_t lru = 0;
    };

    LRUSetAssociativeCache(int size, int num_ways);

    Entry* find(uint64_t key);
    Entry insert(uint64_t key, const Data& data);
    Entry* erase(uint64_t key);
    void rp_promote(uint64_t key);
    void rp_insert(uint64_t key);
    std::string log(const std::vector<std::string>& headers) const;

protected:
    int index_len = 0;

private:
    int num_ways = 1;
    int num_sets = 1;
    uint64_t timestamp = 0;
    std::vector<Entry> entries;
    Entry erased_entry{};

    std::size_t set_index(uint64_t key) const;
};

template <typename Data>
LRUSetAssociativeCache<Data>::LRUSetAssociativeCache(int size, int ways) : num_ways(std::max(1, ways)), entries(std::max(1, size))
{
    num_sets = std::max(1, static_cast<int>(entries.size()) / num_ways);
    while ((1ULL << index_len) < static_cast<uint64_t>(num_sets))
        ++index_len;
}

template <typename Data>
typename LRUSetAssociativeCache<Data>::Entry* LRUSetAssociativeCache<Data>::find(uint64_t key)
{
    auto begin = entries.begin() + static_cast<long>(set_index(key) * num_ways);
    auto end = begin + std::min<std::size_t>(num_ways, static_cast<std::size_t>(entries.end() - begin));
    auto found = std::find_if(begin, end, [key](const auto& entry) { return entry.valid && entry.key == key; });
    return found == end ? nullptr : &*found;
}

template <typename Data>
typename LRUSetAssociativeCache<Data>::Entry LRUSetAssociativeCache<Data>::insert(uint64_t key, const Data& data)
{
    auto begin = entries.begin() + static_cast<long>(set_index(key) * num_ways);
    auto end = begin + std::min<std::size_t>(num_ways, static_cast<std::size_t>(entries.end() - begin));

    auto found = std::find_if(begin, end, [key](const auto& entry) { return entry.valid && entry.key == key; });
    if (found != end) {
        Entry old = *found;
        found->data = data;
        found->lru = ++timestamp;
        return old;
    }

    auto victim = std::find_if(begin, end, [](const auto& entry) { return !entry.valid; });
    if (victim == end)
        victim = std::min_element(begin, end, [](const auto& lhs, const auto& rhs) { return lhs.lru < rhs.lru; });

    Entry old = *victim;
    *victim = {true, key, data, ++timestamp};
    return old;
}

template <typename Data>
typename LRUSetAssociativeCache<Data>::Entry* LRUSetAssociativeCache<Data>::erase(uint64_t key)
{
    Entry* entry = find(key);
    if (!entry)
        return nullptr;

    erased_entry = *entry;
    entry->valid = false;
    return &erased_entry;
}

template <typename Data>
void LRUSetAssociativeCache<Data>::rp_promote(uint64_t key)
{
    if (auto* entry = find(key); entry)
        entry->lru = ++timestamp;
}

template <typename Data>
void LRUSetAssociativeCache<Data>::rp_insert(uint64_t key)
{
    rp_promote(key);
}

template <typename Data>
std::string LRUSetAssociativeCache<Data>::log(const std::vector<std::string>& headers) const
{
    std::ostringstream stream;
    for (const auto& header : headers)
        stream << header << '\t';
    stream << '\n';
    for (const auto& entry : entries) {
        if (entry.valid)
            stream << entry.key << '\n';
    }
    return stream.str();
}

template <typename Data>
std::size_t LRUSetAssociativeCache<Data>::set_index(uint64_t key) const
{
    return static_cast<std::size_t>(hash_index(key, index_len) % static_cast<uint64_t>(num_sets));
}

} // namespace custom_util

namespace gaze_space {

#define __region_offset(block_num) (block_num & REGION_OFFSET_MASK)

#define FT_TYPE custom_util::LRUSetAssociativeCache
#define AT_TYPE custom_util::LRUSetAssociativeCache
#define PT_TYPE custom_util::LRUSetAssociativeCache
#define PB_TYPE custom_util::LRUSetAssociativeCache

constexpr uint64_t REGION_SIZE = 4 * 1024; // '4KB', '8KB', '16KB, '32KB', '64KB, '128KB', '512KB', '1024KB', '2048KB'
constexpr uint64_t LOG2_REGION_SIZE = champsim::lg2(REGION_SIZE);
constexpr uint64_t GAZE_LOG2_BLOCK_SIZE = 6;
constexpr uint64_t GAZE_BLOCK_SIZE = 1ULL << GAZE_LOG2_BLOCK_SIZE;
constexpr uint64_t REGION_OFFSET_MASK = (1ULL << (LOG2_REGION_SIZE - GAZE_LOG2_BLOCK_SIZE)) - 1;

constexpr int NUM_BLOCKS = REGION_SIZE / GAZE_BLOCK_SIZE;

constexpr int FT_SIZE = 64, FT_WAY = 8;
constexpr int AT_SIZE = 64, AT_WAY = 8;
constexpr int PT_WAY = 16;
constexpr int PT_SIZE = PT_WAY * NUM_BLOCKS;
constexpr int PB_SIZE = 32, PB_WAY = 8;

constexpr int STRIDE_PF_LOOKAHEAD = 2;
constexpr int PF_FILL_L1 = 1;
constexpr int PF_FILL_L2 = 2;
constexpr int PF_FILL_L3 = 3;

// ------------------------- Util Functions ------------------------- //
std::pair<float, float> calculate_acc_and_cov(std::vector<int> pattern_1, std::vector<int> pattern_2);
bool different_patterns(std::vector<int> pattern_1, std::vector<int> pattern_2);
std::vector<int> pattern_bool2int(std::vector<bool> pattern);
bool pattern_all_set(std::vector<bool> pattern);
bool pattern_all_set(std::vector<int> pattern);

// ------------------------- Filter Table ------------------------- //
struct FilterTableData {
    uint64_t trigger_offset;
    uint64_t pc;
};

class FilterTable : public FT_TYPE<FilterTableData> {
    typedef FT_TYPE<FilterTableData> Super;

private:
    uint64_t build_key(uint64_t region_num);
    void write_data(Entry& entry, custom_util::Table& table, int row);

public:
    FilterTable(int size, int num_ways);

    Entry* find(uint64_t region_num);
    void insert(uint64_t region_num, uint64_t trigger_offset, uint64_t pc);
    Entry* erase(uint64_t region_num);

    std::string log();
};

// ------------------------- Accumulate Table ------------------------- //
struct AccumulateTableData {
    uint64_t trigger_offset;
    uint64_t second_offset;
    uint64_t pc;
    bool missed_in_pt;
    std::vector<bool> pattern;
    std::vector<int> order;

    int last_stride;
    uint64_t last_offset;

    bool con = false;

    int timestamp = 2;
};

class AccumulateTable : public AT_TYPE<AccumulateTableData> {
    typedef AT_TYPE<AccumulateTableData> Super;

private:
    bool __stride_prefetch = false;

    void write_data(Entry& entry, custom_util::Table& table, int row);
    uint64_t build_key(uint64_t region_num);

public:
    bool get_stride_prefetch();
    void turn_off_stride_prefetch();

public:
    AccumulateTable(int size, int num_ways);

    Entry* set_pattern(uint64_t region_num, uint64_t offset);

    Entry insert(uint64_t region_num, uint64_t trigger_offset, uint64_t second_offset, uint64_t pc, bool missed_in_pt, bool con);
    Entry* erase(uint64_t region_num);

    std::string log();
};

// ------------------------- Pattern Table ------------------------- //
struct PatternTableData {
    std::vector<int> pattern;
    uint64_t pc;
    bool con = false;

    PatternTableData() {
        pattern = std::vector<int>(NUM_BLOCKS, 0);
        pc = 0;
    }
    PatternTableData(std::vector<int> pattern_arg, uint64_t pc_arg) :
        pattern(pattern_arg), pc(pc_arg) {}
    PatternTableData(std::vector<int> pattern_arg, uint64_t pc_arg, bool con_arg) :
        pattern(pattern_arg), pc(pc_arg), con(con_arg) {}
};

class PatternTable : public PT_TYPE<PatternTableData> {
    typedef PT_TYPE<PatternTableData> Super;

private:
    void write_data(Entry& entry, custom_util::Table& table, int row);
    uint64_t build_key(uint64_t trigger, uint64_t second, uint64_t pc, uint64_t region_num);

public:
    std::deque<uint64_t> con_pc;
    int con_counter = 0;

    PatternTable(int size, int num_ways);

    void insert(uint64_t trigger, uint64_t second, uint64_t pc, uint64_t region_num, std::vector<bool> pattern);
    Entry* find(uint64_t trigger, uint64_t second, uint64_t pc, uint64_t region_num);

    std::string log();
};

// ------------------------- Prefetch Buffer ------------------------- //
struct PrefetchBufferData {
public:
    std::vector<int> pattern;
    uint64_t trigger;
    uint64_t second;
    std::vector<int> pf_metadata;
};

struct PrefetchCandidate {
    uint64_t address = 0;
    bool fill_this_level = true;
    uint32_t metadata = 0;
};

class PrefetchBuffer : public PB_TYPE<PrefetchBufferData> {
    typedef PB_TYPE<PrefetchBufferData> Super;

private:
    int pattern_len;

    void write_data(Entry& entry, custom_util::Table& table, int row);
    uint64_t build_key(uint64_t region_num);

public:
    bool warmup;

    PrefetchBuffer(int size, int pattern_len, int debug_level, int num_ways);

    void insert(uint64_t region_num, std::vector<int> pattern, uint64_t trigger, uint64_t second, uint32_t pf_metadata);
    std::vector<PrefetchCandidate> prefetch_candidates(CACHE* cache, uint64_t block_num);
    void prefetch(CACHE* cache, uint64_t block_num);

    std::string log();
};

// ------------------------- Gaze Prefetcher ------------------------- //
class Gaze {
private:
    int cpu;
    int stride_pf_degree = 4;

    FilterTable ft;
    AccumulateTable at;
    PatternTable pt;
    PrefetchBuffer pb;

    PatternTable::Entry* find_in_pt(uint64_t trigger, uint64_t second, uint64_t pc, uint64_t region_num);
    void insert_in_pt(const AccumulateTable::Entry& entry, uint64_t region_num, bool is_end_of_generation, CACHE* cache);

public:
    int global_level = 0;
    bool warmup;

    Gaze(int ft_size, int ft_ways, int at_size, int at_ways, int pt_size, int pt_ways, int pb_size, int pb_ways, int cpu);
    Gaze();
    void set_warmup(bool warmup);

    void access(uint64_t block_num, uint64_t ip, CACHE* cache);
    void eviction(uint64_t block_num, CACHE* cache);
    std::vector<PrefetchCandidate> prefetch_candidates(CACHE* cache, uint64_t block_num);
    void prefetch(CACHE* cache, uint64_t block_num);

    void stride_prefetch(uint64_t block_num);
    void pattern_prefetch(uint64_t trigger, uint64_t second);

    void tune_stride_degree(CACHE* cache);

    void log();
};

} // namespace gaze_space

struct gaze : public champsim::modules::prefetcher {
    explicit gaze(CACHE* cache);

    void prefetcher_initialize();
    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                      uint32_t metadata_in);
    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
    void prefetcher_cycle_operate();
    void prefetcher_final_stats();

private:
    gaze_space::Gaze _prefetcher;
};

#endif