#ifndef PREFETCHER_BOP_HT_H
#define PREFETCHER_BOP_HT_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "address.h"
#include "bop_ht_parameters.h"
#include "modules.h"

namespace bop_ht_space
{

/** Lines whose fill is awaited before their base address is inserted into the RR table.
 *  The key is the line whose fill completes the entry: the prefetched line when a prefetch went
 *  out, or the demand line itself when it did not.
 */
class HoldingTable
{
public:
  explicit HoldingTable(std::size_t size);

  void insert(uint64_t key_line_addr, uint64_t base_addr);

  /** Remove and return the base address held for @key_line_addr, if any */
  std::optional<uint64_t> take(uint64_t key_line_addr);

private:
  struct Entry
  {
    bool valid = false;
    uint64_t key_line_addr = 0;
    uint64_t base_addr = 0;
  };

  std::vector<Entry> entries;
  std::size_t eviction_idx = 0; // Round-robin victim when full
};

} // namespace bop_ht_space

class bop_ht : public champsim::modules::prefetcher
{
public:
  explicit bop_ht(CACHE* cache);

  /** Hardware prefetcher enabled */
  bool issuePrefetchRequests = false;

  /** Filter the access, train on it and return the prefetch address (if any) for it.
   *  A trained access that yields no prefetch is resolved here via the demand fallback; a returned
   *  prefetch must be resolved by the caller through prefetchOutcome().
   *  Shared by the standalone module and the pumpkin wrapper.
   */
  std::optional<champsim::address> operate(champsim::address addr, uint8_t cache_hit, bool useful_prefetch, access_type type);

  /** Resolve a prefetch returned by operate()
   *  @param pf_addr: the prefetch address
   *  @param base_addr: the access that triggered it
   *  @param base_cache_hit: the triggering access hit in the cache
   *  @param filtered_out: the prefetch was dropped by a filter before reaching the cache
   *  @param pf_in_cache: the prefetched line is already resident, so no fill will follow
   */
  void prefetchOutcome(uint64_t pf_addr, uint64_t base_addr, bool base_cache_hit, bool filtered_out, bool pf_in_cache);

  /** Learning phase of the BOP. Update the intermediate values of the
   * round and update the best offset if found
   * @param addr: full address used to compute X-O tag to determine
   *              offset efficacy.
   */
  void bestOffsetLearning(uint64_t addr);

  std::optional<uint64_t> calculatePrefetchAddr(uint64_t addr);

  /** Complete the holding table entry (if any) of the filled line */
  void insertFill(uint64_t addr);

  // ChampSim prefetcher interface
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);

private:
  /** Learning phase parameters */
  unsigned int scoreMax = bop_ht_space::BOP_SCORE_MAX;
  unsigned int roundMax = bop_ht_space::BOP_ROUND_MAX;
  unsigned int badScore = bop_ht_space::BOP_BAD_SCORE;
  /** Recent requests table parameteres */
  unsigned int rrEntries = bop_ht_space::BOP_RR_SIZE;
  unsigned int tagMask = (1u << bop_ht_space::BOP_TAG_BITS) - 1;

  std::vector<uint64_t> rrTable;

  bop_ht_space::HoldingTable holdingTable{bop_ht_space::BOP_HOLDING_TABLE_SIZE};

  /** Structure to save the offset and the score */
  using OffsetListEntry = std::pair<int64_t, uint8_t>;
  std::vector<OffsetListEntry> offsetsList;

  /** Current best offset to issue prefetches */
  int64_t bestOffset = 0;
  /** Current best offset found in the learning phase */
  int64_t phaseBestOffset = 0;
  /** Current test offset index */
  std::size_t offsetsListIndex = 0;
  /** Max score found so far */
  unsigned int bestScore = 0;
  /** Current round */
  unsigned int round = 0;

  /** Generate a hash for the specified address to index the RR table
   *  @param addr: address to hash
   */
  unsigned int index(uint64_t addr) const;

  /** Insert the specified address into the RR table
   *  @param addr: address to insert
   *  @param addr_tag: The tag to insert in the RR table
   */
  void insertIntoRR(uint64_t addr, uint64_t addr_tag);

  /** No prefetch reaches the cache for this trained access: record the demand itself, either
   *  straight into the RR table (the line is resident) or via the holding table until it fills.
   */
  void demandFallback(uint64_t addr, bool cache_hit);

  /** Reset all the scores from the offset list */
  void resetScores();

  /** Generate the tag for the specified address based on the tag bits
   *  and the block size
   *  @param addr: address to get the tag from
   */
  uint64_t tag(uint64_t addr) const;

  /** Test if @X-O is hitting in the RR table to update the
   *  offset score
   *  @param addr_tag: tag searched for within the RR
   */
  bool testRR(uint64_t addr_tag) const;
};

#endif
