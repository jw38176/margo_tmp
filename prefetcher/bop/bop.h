#ifndef PREFETCHER_BOP_H
#define PREFETCHER_BOP_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "address.h"
#include "bop_parameters.h"
#include "modules.h"

class bop : public champsim::modules::prefetcher
{
public:
  explicit bop(CACHE* cache);

  /** Hardware prefetcher enabled */
  bool issuePrefetchRequests = false;

  /** Filter the access, train on it and return the prefetch address (if any) for it.
   *  Shared by the standalone module and the pumpkin wrapper.
   */
  std::optional<champsim::address> operate(champsim::address addr, uint8_t cache_hit, bool useful_prefetch, access_type type);

  /** Learning phase of the BOP. Update the intermediate values of the
   * round and update the best offset if found
   * @param addr: full address used to compute X-O tag to determine
   *              offset efficacy.
   */
  void bestOffsetLearning(uint64_t addr);

  std::optional<uint64_t> calculatePrefetchAddr(uint64_t addr);

  /** @param bop_prefetch: the filled line was brought in by a BOP prefetch */
  void insertFill(uint64_t addr, bool prefetch, bool bop_prefetch);

  // ChampSim prefetcher interface
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);

private:
  /** Learning phase parameters */
  unsigned int scoreMax = bop_space::BOP_SCORE_MAX;
  unsigned int roundMax = bop_space::BOP_ROUND_MAX;
  unsigned int badScore = bop_space::BOP_BAD_SCORE;
  /** Recent requests table parameteres */
  unsigned int rrEntries = bop_space::BOP_RR_SIZE;
  unsigned int tagMask = (1u << bop_space::BOP_TAG_BITS) - 1;

  std::vector<uint64_t> rrTable;

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
