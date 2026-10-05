#include "bop_ht.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "cache.h"
#include "champsim.h"
#include "msl/bits.h"

using namespace bop_ht_space;

/////////////////////////////////////////////////////////////
// Holding Table
/////////////////////////////////////////////////////////////

HoldingTable::HoldingTable(std::size_t size) : entries(size) {}

void HoldingTable::insert(uint64_t key_line_addr, uint64_t base_addr)
{
  auto matches = [key_line_addr](const Entry& e) { return e.valid && e.key_line_addr == key_line_addr; };
  if (std::any_of(entries.begin(), entries.end(), matches)) {
    if constexpr (BOP_DEBUG) {
      std::cout << "Holding table insert skipped - duplicate line " << key_line_addr << std::endl;
    }
    return;
  }

  auto slot = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return !e.valid; });
  if (slot == entries.end()) {
    slot = entries.begin() + static_cast<std::ptrdiff_t>(eviction_idx);
    eviction_idx = (eviction_idx + 1) % entries.size();
  }
  *slot = {true, key_line_addr, base_addr};
}

std::optional<uint64_t> HoldingTable::take(uint64_t key_line_addr)
{
  auto it = std::find_if(entries.begin(), entries.end(), [key_line_addr](const Entry& e) { return e.valid && e.key_line_addr == key_line_addr; });
  if (it == entries.end()) {
    return std::nullopt;
  }
  it->valid = false;
  return it->base_addr;
}

/////////////////////////////////////////////////////////////
// bop_ht prefetcher
/////////////////////////////////////////////////////////////

bop_ht::bop_ht(CACHE* cache) : prefetcher(cache)
{
  if (!champsim::msl::is_power_of_2(rrEntries)) {
    throw std::invalid_argument{"Number of RR entries is not power of 2\n"};
  }
  if (!champsim::msl::is_power_of_2(BLOCK_SIZE)) {
    throw std::invalid_argument{"Cache line size is not power of 2\n"};
  }
  if (BOP_NEGATIVE_OFFSETS_ENABLE && (BOP_OFFSET_LIST_SIZE % 2 != 0)) {
    throw std::invalid_argument{"Negative offsets enabled with odd offset list size\n"};
  }
  if (BOP_HOLDING_TABLE_SIZE == 0) {
    throw std::invalid_argument{"Holding table must have at least one entry\n"};
  }

  rrTable.resize(rrEntries);

  /*
   * Following the paper implementation, a list with the specified number
   * of offsets which are of the form 2^i * 3^j * 5^k with i,j,k >= 0
   */
  const int factors[] = {2, 3, 5};
  unsigned int i = 0;
  int64_t offset_i = 1;

  while (i < BOP_OFFSET_LIST_SIZE) {
    int64_t offset = offset_i;

    for (int n : factors) {
      while ((offset % n) == 0) {
        offset /= n;
      }
    }

    if (offset == 1) {
      offsetsList.push_back(OffsetListEntry(offset_i, 0));
      i++;
      /*
       * If we want to use negative offsets, add also the negative value
       * of the offset just calculated
       */
      if (BOP_NEGATIVE_OFFSETS_ENABLE) {
        offsetsList.push_back(OffsetListEntry(-offset_i, 0));
        i++;
      }
    }

    offset_i++;
  }

  offsetsListIndex = 0;
}

unsigned int bop_ht::index(uint64_t addr) const
{
  /*
   * For indexing the RR Table, the cache line
   * address is XORed with itself after right shifting it by the log base
   * 2 of the number of entries in the RR Table.
   */
  uint64_t log_rr_entries = champsim::msl::lg2(rrEntries);
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  uint64_t hash = line_addr ^ (line_addr >> log_rr_entries);
  hash &= ((1ULL << log_rr_entries) - 1);
  return static_cast<unsigned int>(hash % rrEntries);
}

void bop_ht::insertIntoRR(uint64_t addr, uint64_t tag) { rrTable[index(addr)] = tag; }

void bop_ht::resetScores()
{
  for (auto& it : offsetsList) {
    it.second = 0;
  }
}

uint64_t bop_ht::tag(uint64_t addr) const
{
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  return line_addr & tagMask;
}

bool bop_ht::testRR(uint64_t addr_tag) const
{
  for (auto& it : rrTable) {
    if (it == addr_tag) {
      return true;
    }
  }

  return false;
}

void bop_ht::bestOffsetLearning(uint64_t addr)
{
  auto& entry = offsetsList[offsetsListIndex];
  int64_t offset = entry.first;

  /*
   * Compute the lookup tag for the RR table. As tags are generated using
   * lower 12 bits we subtract offset from the full address rather than the
   * tag to avoid integer underflow.
   */
  uint64_t lookup_tag = tag(addr - (static_cast<uint64_t>(offset) << LOG2_BLOCK_SIZE));

  // There was a hit in the RR table, increment the score for this offset
  if (testRR(lookup_tag)) {
    if constexpr (BOP_DEBUG) {
      std::cout << "Tag " << lookup_tag << " found in the RR table" << std::endl;
    }
    entry.second++;
    if (entry.second > bestScore) {
      bestScore = entry.second;
      phaseBestOffset = entry.first;
      if constexpr (BOP_DEBUG) {
        std::cout << "New best score is " << bestScore << std::endl;
      }
    }
  }

  // Move the offset iterator forward to prepare for the next time
  offsetsListIndex++;

  /*
   * All the offsets in the list were visited meaning that a learning
   * phase finished. Check if
   */
  if (offsetsListIndex == offsetsList.size()) {
    offsetsListIndex = 0;
    round++;
  }

  // Check if its time to re-calculate the best offset
  if ((bestScore >= scoreMax) || (round >= roundMax)) {
    round = 0;

    /*
     * If the current best score (bestScore) has exceed the threshold to
     * enable prefetching (badScore), reset the learning structures and
     * enable prefetch generation
     */
    if (bestScore > badScore) {
      bestOffset = phaseBestOffset;
      if constexpr (BOP_DEBUG) {
        std::cout << "New best offset is " << bestOffset << std::endl;
      }
      round = 0;
      issuePrefetchRequests = true;
    } else {
      issuePrefetchRequests = false;
    }
    resetScores();
    bestScore = 0;
    phaseBestOffset = 0;
  }
}

std::optional<uint64_t> bop_ht::calculatePrefetchAddr(uint64_t addr)
{
  uint64_t pf_addr = addr + (static_cast<uint64_t>(bestOffset) << LOG2_BLOCK_SIZE);

  if ((addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
    if constexpr (BOP_DEBUG) {
      std::cout << "Prefetch not issued - Page crossed" << std::endl;
    }
    return std::nullopt;
  }

  if constexpr (BOP_DEBUG) {
    std::cout << "Generated prefetch: " << pf_addr << std::endl;
  }
  return pf_addr;
}

void bop_ht::demandFallback(uint64_t addr, bool cache_hit)
{
  if (cache_hit) {
    insertIntoRR(addr, tag(addr));
  } else {
    holdingTable.insert(addr >> LOG2_BLOCK_SIZE, addr);
  }
}

void bop_ht::prefetchOutcome(uint64_t pf_addr, uint64_t base_addr, bool base_cache_hit, bool filtered_out, bool pf_in_cache)
{
  if (filtered_out) {
    demandFallback(base_addr, base_cache_hit);
  } else if (pf_in_cache) {
    insertIntoRR(pf_addr, tag(base_addr));
  } else {
    holdingTable.insert(pf_addr >> LOG2_BLOCK_SIZE, base_addr);
  }
}

void bop_ht::insertFill(uint64_t addr)
{
  if (auto base_addr = holdingTable.take(addr >> LOG2_BLOCK_SIZE); base_addr.has_value()) {
    insertIntoRR(addr, tag(*base_addr));
  }
}

std::optional<champsim::address> bop_ht::operate(champsim::address addr, uint8_t cache_hit, bool useful_prefetch, access_type type)
{
  if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
    return std::nullopt; // Not a load or prefetch from L1
  }

  // Skip duplicate demand misses: if this miss hits an in-flight non-prefetch
  // MSHR entry, it's a duplicate and shouldn't notify the prefetcher.
  // Only MSHR hits on prefetch entries (late prefetches) are real misses.
  if (!cache_hit) {
    auto block_addr = champsim::block_number{addr};
    auto mshr_entry = std::find_if(intern_->MSHR.begin(), intern_->MSHR.end(),
                                   [block_addr](const auto& entry) { return champsim::block_number{entry.address} == block_addr; });
    if (mshr_entry != intern_->MSHR.end() && mshr_entry->type != access_type::PREFETCH) {
      return std::nullopt;
    }
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {
    /*
     * Go through the nth offset and update the score, the best score and the
     * current best offset if a better one is found
     */
    bestOffsetLearning(addr.to<uint64_t>());

    if (issuePrefetchRequests) {
      auto pf_addr = calculatePrefetchAddr(addr.to<uint64_t>());
      if (pf_addr.has_value()) {
        return champsim::address{*pf_addr};
      }
    }

    demandFallback(addr.to<uint64_t>(), cache_hit);
  }

  return std::nullopt;
}

void bop_ht::prefetcher_initialize() { std::cout << "BOP-HT Prefetcher Initialise" << std::endl; }

// addr: address of cache block
// ip: PC of load/store
// cache_hit: true if load/store hits in cache.
uint32_t bop_ht::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                          uint32_t metadata_in)
{
  auto pf_addr = operate(addr, cache_hit, useful_prefetch, type);
  if (pf_addr.has_value()) {
    const bool pf_in_cache = intern_->in_cache(*pf_addr);
    prefetch_line(*pf_addr, true, 0x1);
    prefetchOutcome(pf_addr->to<uint64_t>(), addr.to<uint64_t>(), cache_hit, false, pf_in_cache);
  }

  return metadata_in;
}

// Invoked when cache is refilled (prefetch or not)
// addr: address of cache line that was prefetched.
// set and way of allocated entry
uint32_t bop_ht::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  insertFill(addr.to<uint64_t>());

  return metadata_in;
}
