#ifndef PREFETCHER_SPP_PPF_H
#define PREFETCHER_SPP_PPF_H

/*
 * SPP-PPF (Signature Path Prefetcher with Perceptron-based Prefetch Filtering)
 *
 * Ported from the legacy (function-style) ChampSim implementation to the current
 * class-based champsim::modules::prefetcher interface. The prefetcher can be used
 * either as a standalone ChampSim prefetcher module, or as a Pumpkin sub-prefetcher
 * (see pumpkin_wrapper.{h,cc}).
 *
 * The candidate-generation logic is factored into spp_ppf::generate() so that both the
 * standalone path and the Pumpkin wrapper share the exact same training/lookahead code.
 * Issuing a prefetch (and the bookkeeping that depends on it, e.g. updating the prefetch
 * filter) is deferred to spp_ppf::on_issue(), which the standalone path invokes when
 * prefetch_line() succeeds and the Pumpkin wrapper invokes from its notify_result callback.
 */

#include <cstdint>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "modules.h"

// SPP functional knobs
#define LOOKAHEAD_ON
#define FILTER_ON
#define GHR_ON
#define SPP_SANITY_CHECK

//#define SPP_DEBUG_PRINT
#ifdef SPP_DEBUG_PRINT
#define SPP_DP(x) x
#else
#define SPP_DP(x)
#endif

//#define SPP_PERC_WGHT
#ifdef SPP_PERC_WGHT
#define SPP_PW(x) x
#else
#define SPP_PW(x)
#endif

// Signature table parameters
#define ST_SET 1
#define ST_WAY 256
#define ST_TAG_BIT 16
#define ST_TAG_MASK ((1 << ST_TAG_BIT) - 1)
#define SIG_SHIFT 3
#define SIG_BIT 12
#define SIG_MASK ((1 << SIG_BIT) - 1)
#define SIG_DELTA_BIT 7

// Pattern table parameters
#define PT_SET 512
#define PT_WAY 4
#define C_SIG_BIT 4
#define C_DELTA_BIT 4
#define C_SIG_MAX ((1 << C_SIG_BIT) - 1)
#define C_DELTA_MAX ((1 << C_DELTA_BIT) - 1)

// Prefetch filter parameters
#define QUOTIENT_BIT 10
#define REMAINDER_BIT 6
#define HASH_BIT (QUOTIENT_BIT + REMAINDER_BIT + 1)
#define FILTER_SET (1 << QUOTIENT_BIT)

#define QUOTIENT_BIT_REJ 10
#define REMAINDER_BIT_REJ 8
#define HASH_BIT_REJ (QUOTIENT_BIT_REJ + REMAINDER_BIT_REJ + 1)
#define FILTER_SET_REJ (1 << QUOTIENT_BIT_REJ)

// Global register parameters
#define GLOBAL_COUNTER_BIT 10
#define GLOBAL_COUNTER_MAX ((1 << GLOBAL_COUNTER_BIT) - 1)
#define MAX_GHR_ENTRY 8
#define PAGES_TRACKED 6

// Perceptron paramaters
#define PERC_ENTRIES 4096   // Upto 12-bit addressing in hashed perceptron
#define PERC_FEATURES 9     // Keep increasing based on new features
#define PERC_COUNTER_MAX 15 //-16 to +15: 5 bits counter
#define PERC_THRESHOLD_HI -5
#define PERC_THRESHOLD_LO -15
#define POS_UPDT_THRESHOLD 90
#define NEG_UPDT_THRESHOLD -80

struct spp_ppf;

namespace spp_ppf_space
{

enum FILTER_REQUEST { SPP_L2C_PREFETCH, SPP_LLC_PREFETCH, L2C_DEMAND, L2C_EVICT, SPP_PERC_REJECT }; // Request type for prefetch filter

uint64_t get_hash(uint64_t key);

class SIGNATURE_TABLE
{
public:
  spp_ppf* _parent = nullptr;

  bool valid[ST_SET][ST_WAY];
  uint32_t tag[ST_SET][ST_WAY], last_offset[ST_SET][ST_WAY], sig[ST_SET][ST_WAY], lru[ST_SET][ST_WAY];

  SIGNATURE_TABLE()
  {
    for (uint32_t set = 0; set < ST_SET; set++)
      for (uint32_t way = 0; way < ST_WAY; way++) {
        valid[set][way] = 0;
        tag[set][way] = 0;
        last_offset[set][way] = 0;
        sig[set][way] = 0;
        lru[set][way] = way;
      }
  }

  void read_and_update_sig(uint64_t page, uint32_t page_offset, uint32_t& last_sig, uint32_t& curr_sig, int32_t& delta);
};

class PATTERN_TABLE
{
public:
  spp_ppf* _parent = nullptr;

  int delta[PT_SET][PT_WAY];
  uint32_t c_delta[PT_SET][PT_WAY], c_sig[PT_SET];

  PATTERN_TABLE()
  {
    for (uint32_t set = 0; set < PT_SET; set++) {
      for (uint32_t way = 0; way < PT_WAY; way++) {
        delta[set][way] = 0;
        c_delta[set][way] = 0;
      }
      c_sig[set] = 0;
    }
  }

  void update_pattern(uint32_t last_sig, int curr_delta);
  void read_pattern(uint32_t curr_sig, int* prefetch_delta, uint32_t* confidence_q, int32_t* perc_sum_q, uint32_t& lookahead_way, uint32_t& lookahead_conf,
                    uint32_t& pf_q_tail, uint32_t& depth, uint64_t addr, uint64_t base_addr, uint64_t train_addr, uint64_t curr_ip, int32_t train_delta,
                    uint32_t last_sig, uint32_t pq_occupancy, uint32_t pq_SIZE, uint32_t mshr_occupancy, uint32_t mshr_SIZE);
};

class PREFETCH_FILTER
{
public:
  spp_ppf* _parent = nullptr;

  uint64_t remainder_tag[FILTER_SET], pc[FILTER_SET], pc_1[FILTER_SET], pc_2[FILTER_SET], pc_3[FILTER_SET], address[FILTER_SET];
  bool valid[FILTER_SET], // Consider this as "prefetched"
      useful[FILTER_SET]; // Consider this as "used"
  int32_t delta[FILTER_SET], perc_sum[FILTER_SET];
  uint32_t last_signature[FILTER_SET], confidence[FILTER_SET], cur_signature[FILTER_SET], la_depth[FILTER_SET];

  uint64_t remainder_tag_reject[FILTER_SET_REJ], pc_reject[FILTER_SET_REJ], pc_1_reject[FILTER_SET_REJ], pc_2_reject[FILTER_SET_REJ], pc_3_reject[FILTER_SET_REJ],
      address_reject[FILTER_SET_REJ];
  bool valid_reject[FILTER_SET_REJ]; // Entries which the perceptron rejected
  int32_t delta_reject[FILTER_SET_REJ], perc_sum_reject[FILTER_SET_REJ];
  uint32_t last_signature_reject[FILTER_SET_REJ], confidence_reject[FILTER_SET_REJ], cur_signature_reject[FILTER_SET_REJ], la_depth_reject[FILTER_SET_REJ];

  // To enable / disable negative training using reject filter
  bool train_neg;

  float hist_hits[55];
  float hist_tots[55];

  PREFETCH_FILTER()
  {
    for (int i = 0; i < 55; i++) {
      hist_hits[i] = 0;
      hist_tots[i] = 0;
    }
    for (uint32_t set = 0; set < FILTER_SET; set++) {
      remainder_tag[set] = 0;
      valid[set] = 0;
      useful[set] = 0;
    }
    for (uint32_t set = 0; set < FILTER_SET_REJ; set++) {
      valid_reject[set] = 0;
      remainder_tag_reject[set] = 0;
    }
    train_neg = 0;
  }

  bool check(uint64_t pf_addr, uint64_t base_addr, uint64_t ip, FILTER_REQUEST filter_request, int32_t cur_delta, uint32_t last_sign, uint32_t cur_sign,
             uint32_t confidence, int32_t sum, uint32_t depth);
  bool add_to_filter(uint64_t check_addr, uint64_t base_addr, uint64_t ip, FILTER_REQUEST filter_request, int cur_delta, uint32_t last_sig, uint32_t curr_sig,
                     uint32_t conf, int32_t sum, uint32_t depth);
};

class PERCEPTRON
{
public:
  spp_ppf* _parent = nullptr;

  // Perc Weights
  int32_t perc_weights[PERC_ENTRIES][PERC_FEATURES];

  // Only for dumping csv
  bool perc_touched[PERC_ENTRIES][PERC_FEATURES];

  // CONST depths for different features
  int32_t PERC_DEPTH[PERC_FEATURES];

  PERCEPTRON()
  {
    PERC_DEPTH[0] = PERC_ENTRIES / 2; // base_addr;
    PERC_DEPTH[1] = PERC_ENTRIES;     // cache_line;
    PERC_DEPTH[2] = PERC_ENTRIES;     // page_addr;
    PERC_DEPTH[3] = PERC_ENTRIES;     // confidence ^ page_addr;
    PERC_DEPTH[4] = PERC_ENTRIES / 4; // curr_sig ^ sig_delta;
    PERC_DEPTH[5] = PERC_ENTRIES;     // ip_1 ^ ip_2 ^ ip_3;
    PERC_DEPTH[6] = PERC_ENTRIES / 4; // ip ^ depth;
    PERC_DEPTH[7] = PERC_ENTRIES / 2; // ip ^ sig_delta;
    PERC_DEPTH[8] = PERC_ENTRIES / 32; // confidence;

    for (int i = 0; i < PERC_ENTRIES; i++) {
      for (int j = 0; j < PERC_FEATURES; j++) {
        perc_weights[i][j] = 0;
        perc_touched[i][j] = 0;
      }
    }
  }

  void get_perc_index(uint64_t base_addr, uint64_t ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3, int32_t cur_delta, uint32_t last_sig, uint32_t curr_sig,
                      uint32_t confidence, uint32_t depth, uint64_t perc_set[PERC_FEATURES]);
  void perc_update(uint64_t check_addr, uint64_t ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3, int32_t cur_delta, uint32_t last_sig, uint32_t curr_sig,
                   uint32_t confidence, uint32_t depth, bool direction, int32_t perc_sum);
  int32_t perc_predict(uint64_t check_addr, uint64_t ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3, int32_t cur_delta, uint32_t last_sig, uint32_t curr_sig,
                       uint32_t confidence, uint32_t depth);
};

class GLOBAL_REGISTER
{
public:
  spp_ppf* _parent = nullptr;

  // Global counters to calculate global prefetching accuracy
  uint64_t pf_useful, pf_issued, global_accuracy; // Alpha value in Section III. Equation 3

  // Global History Register (GHR) entries
  uint8_t valid[MAX_GHR_ENTRY];
  uint32_t sig[MAX_GHR_ENTRY], confidence[MAX_GHR_ENTRY], offset[MAX_GHR_ENTRY];
  int delta[MAX_GHR_ENTRY];

  uint64_t ip_0, ip_1, ip_2, ip_3;

  uint64_t page_tracker[PAGES_TRACKED];

  // Stats Collection
  double depth_val, depth_sum, depth_num;
  double pf_total, pf_l2c, pf_llc, pf_l2c_good;
  long perc_pass, perc_reject, reject_update;

  GLOBAL_REGISTER()
  {
    pf_useful = 0;
    pf_issued = 0;
    global_accuracy = 0;
    ip_0 = 0;
    ip_1 = 0;
    ip_2 = 0;
    ip_3 = 0;

    depth_val = 0;
    depth_sum = 0;
    depth_num = 0;
    pf_total = 0;
    pf_l2c = 0;
    pf_llc = 0;
    pf_l2c_good = 0;
    perc_pass = 0;
    perc_reject = 0;
    reject_update = 0;

    for (uint32_t i = 0; i < PAGES_TRACKED; i++)
      page_tracker[i] = 0;

    for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
      valid[i] = 0;
      sig[i] = 0;
      confidence[i] = 0;
      offset[i] = 0;
      delta[i] = 0;
    }
  }

  void update_entry(uint32_t pf_sig, uint32_t pf_confidence, uint32_t pf_offset, int pf_delta);
  uint32_t check_entry(uint32_t page_offset);
};

// A prefetch candidate produced by spp_ppf::generate(). The "commit" fields carry the
// information required to update the prefetch filter once the prefetch is actually issued.
struct PrefetchRequest {
  uint64_t pf_addr = 0;
  bool fill_this_level = true;
  uint32_t metadata = 0;

  // Deferred filter-update payload (see spp_ppf::on_issue)
  uint64_t train_addr = 0;
  uint64_t curr_ip = 0;
  FILTER_REQUEST fill_level = SPP_L2C_PREFETCH;
  int32_t delta = 0;
  uint32_t last_sig = 0;
  uint32_t curr_sig = 0;
  uint32_t confidence = 0;
  int32_t perc_sum = 0;
  uint32_t depth = 0;
};

} // namespace spp_ppf_space

struct spp_ppf : public champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  static uint64_t get_hash(uint64_t key);

  // ChampSim prefetcher interface (standalone use)
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();

  // Shared candidate-generation API (used by the standalone path and the Pumpkin wrapper)
  std::vector<spp_ppf_space::PrefetchRequest> generate(uint64_t addr, uint64_t ip, uint32_t pq_occupancy, uint32_t pq_size, uint32_t mshr_occupancy,
                                                       uint32_t mshr_size, bool warmup);
  void on_issue(const spp_ppf_space::PrefetchRequest& req);
  void notify_fill_evict(uint64_t evicted_addr);

  void init_tables();

  // SPP-PPF state
  spp_ppf_space::SIGNATURE_TABLE ST;
  spp_ppf_space::PATTERN_TABLE PT;
  spp_ppf_space::PREFETCH_FILTER FILTER;
  spp_ppf_space::GLOBAL_REGISTER GHR;
  spp_ppf_space::PERCEPTRON PERC;

  std::size_t L2C_MSHR_SIZE = 0;

  int depth_track[30] = {};
  int prefetch_q_full = 0;

  // Neelu: Access counters for all tables and a warmup flag for l2
  uint64_t ST_read_accesses = 0, ST_write_accesses = 0, PT_read_accesses = 0, PT_write_accesses = 0, FILTER_read_accesses = 0, FILTER_write_accesses = 0,
           GHR_read_accesses = 0, GHR_write_accesses = 0, PERC_read_accesses = 0, PERC_write_accesses = 0, GHR_counter_read_accesses = 0,
           GHR_counter_write_accesses = 0, GHR_ip_read_accesses = 0, GHR_ip_write_accesses = 0;
  uint64_t ST_tag_read_accesses = 0, ST_tag_write_accesses = 0;

  uint8_t warmup_flag_l2 = 0;
};

#endif
