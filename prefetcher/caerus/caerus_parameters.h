# ifndef _CAERUS_PARAMETERS_H_
# define _CAERUS_PARAMETERS_H_

namespace caerus_space
{ 
  /*****************************************************************************
   *                              PARAMS                                       *
   *****************************************************************************/
  // CAERUS

  # define ADDR_TAG_BITS        (14)  // Bits used to store the tag

  // OFFSET TRAINING
  # define SCORE_MAX            (24)  // Max. score to update the best offset
  # define ROUND_MAX            (500) // Max. round to update the best offset
  # define BAD_SCORE            (10)   // Score at which to not consider any offset
  # define OFFSET_LIST_SIZE     (104)  // Number of entries in the offsets list
  # define NUM_OFFSETS          (20)   // The number of offsets that are learnt

  // ACCURACY TRACKING
  # define ACCURACY_INCREMENT   (1)   // Increment value for accuracy table
  # define ACCURACY_DECREMENT   (1)   // Decrement value for accuracy table
  # define ACCURACY_THRESHOLD   (10)   // Saturing counter value for pc/offset to be accurate

  // HARDWARE STRUCTURES
  # define RR_SIZE              (32)  // Number of shift registers in the RR Table
  # define RR_ENTRY_SIZE        (16)   // Size of each fifo shift register
  # define HOLDING_TABLE_SIZE   (64) // Size of the holding table
  # define HOLDING_TABLE_ASSERT_ON_FULL (false) // If true, assert on full; if false, evict oldest entry
  # define ACCURACY_TABLE_SIZE  (128) // Size of the pc accuracy table
  
  # define NUMERIC_OFFSETS_ENABLE (false) // If enabled, the offsets will be full-range numeric offsets

  # define ALLOW_CROSS_PAGE_PREFETCH (true) // Determines if prefetches are allowed to cross page boundaries
  # define NEGATIVE_OFFSETS_ENABLE (true) // If enabled, half of the offset list will contain negative offsets
};
# endif
