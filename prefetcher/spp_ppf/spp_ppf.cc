#include "spp_ppf.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>

#include "cache.h"
#include "champsim.h"

using namespace spp_ppf_space;

namespace spp_ppf_space
{

// TODO: Find a good 64-bit hash function
uint64_t get_hash(uint64_t key)
{
  // Robert Jenkins' 32 bit mix function
  key += (key << 12);
  key ^= (key >> 22);
  key += (key << 4);
  key ^= (key >> 9);
  key += (key << 10);
  key ^= (key >> 2);
  key += (key << 7);
  key ^= (key >> 12);

  // Knuth's multiplicative method
  key = (key >> 3) * 2654435761;

  return key;
}

void SIGNATURE_TABLE::read_and_update_sig(uint64_t page, uint32_t page_offset, uint32_t& last_sig, uint32_t& curr_sig, int32_t& delta)
{
  uint32_t set = get_hash(page) % ST_SET, match = ST_WAY, partial_page = page & ST_TAG_MASK;
  uint8_t ST_hit = 0;
  int sig_delta = 0;

  SPP_DP(std::cout << "[ST] " << __func__ << " page: " << std::hex << page << " partial_page: " << partial_page << std::dec << std::endl;);

  // Case 1: Hit
  for (match = 0; match < ST_WAY; match++) {
    _parent->ST_tag_read_accesses++;

    if (valid[set][match] && (tag[set][match] == partial_page)) {
      _parent->ST_read_accesses++;
      last_sig = sig[set][match];
      delta = page_offset - last_offset[set][match];

      if (delta) {
        // Build a new sig based on 7-bit sign magnitude representation of delta
        sig_delta = (delta < 0) ? (((-1) * delta) + (1 << (SIG_DELTA_BIT - 1))) : delta;
        sig[set][match] = ((last_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
        curr_sig = sig[set][match];
        last_offset[set][match] = page_offset;
      } else
        last_sig = 0; // Hitting the same cache line, delta is zero

      ST_hit = 1;
      break;
    }
  }

  // Case 2: Invalid
  if (match == ST_WAY) {
    for (match = 0; match < ST_WAY; match++) {
      _parent->ST_tag_read_accesses++;

      if (valid[set][match] == 0) {
        _parent->ST_write_accesses++;

        valid[set][match] = 1;
        tag[set][match] = partial_page;
        sig[set][match] = 0;
        curr_sig = sig[set][match];
        last_offset[set][match] = page_offset;

        break;
      }
    }
  }

  // Case 3: Miss
  if (match == ST_WAY) {
    for (match = 0; match < ST_WAY; match++) {
      _parent->ST_tag_read_accesses++;

      if (lru[set][match] == ST_WAY - 1) { // Find replacement victim
        _parent->ST_write_accesses++;

        tag[set][match] = partial_page;
        sig[set][match] = 0;
        curr_sig = sig[set][match];
        last_offset[set][match] = page_offset;

        break;
      }
    }

#ifdef SPP_SANITY_CHECK
    if (match == ST_WAY) {
      std::cout << "[ST] Cannot find a replacement victim!" << std::endl;
      assert(0);
    }
#endif
  }

#ifdef GHR_ON
  if (ST_hit == 0) {
    uint32_t GHR_found = _parent->GHR.check_entry(page_offset);
    if (GHR_found < MAX_GHR_ENTRY) {
      _parent->GHR_read_accesses++;
      sig_delta = (_parent->GHR.delta[GHR_found] < 0) ? (((-1) * _parent->GHR.delta[GHR_found]) + (1 << (SIG_DELTA_BIT - 1))) : _parent->GHR.delta[GHR_found];
      sig[set][match] = ((_parent->GHR.sig[GHR_found] << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
      curr_sig = sig[set][match];
    }
  }
#endif

  // Update LRU
  for (uint32_t way = 0; way < ST_WAY; way++) {
    _parent->ST_tag_read_accesses++;

    if (lru[set][way] < lru[set][match]) {
      lru[set][way]++;

      _parent->ST_tag_write_accesses++;

#ifdef SPP_SANITY_CHECK
      if (lru[set][way] >= ST_WAY) {
        std::cout << "[ST] LRU value is wrong! set: " << set << " way: " << way << " lru: " << lru[set][way] << std::endl;
        assert(0);
      }
#endif
    }
  }
  _parent->ST_tag_write_accesses++;
  lru[set][match] = 0; // Promote to the MRU position
}

void PATTERN_TABLE::update_pattern(uint32_t last_sig, int curr_delta)
{
  // Update (sig, delta) correlation
  uint32_t set = get_hash(last_sig) % PT_SET, match = 0;

  // Case 1: Hit
  for (match = 0; match < PT_WAY; match++) {
    _parent->PT_read_accesses++;

    if (delta[set][match] == curr_delta) {
      _parent->PT_write_accesses++;

      c_delta[set][match]++;
      c_sig[set]++;
      if (c_sig[set] > C_SIG_MAX) {
        _parent->PT_write_accesses += PT_WAY;
        for (uint32_t way = 0; way < PT_WAY; way++)
          c_delta[set][way] >>= 1;
        c_sig[set] >>= 1;
      }

      break;
    }
  }

  // Case 2: Miss
  if (match == PT_WAY) {
    uint32_t victim_way = PT_WAY, min_counter = C_SIG_MAX;

    for (match = 0; match < PT_WAY; match++) {
      _parent->PT_read_accesses++;

      if (c_delta[set][match] < min_counter) { // Select an entry with the minimum c_delta
        victim_way = match;
        min_counter = c_delta[set][match];
      }
    }

    _parent->PT_write_accesses++;

    delta[set][victim_way] = curr_delta;
    c_delta[set][victim_way] = 0;
    c_sig[set]++;
    if (c_sig[set] > C_SIG_MAX) {
      _parent->PT_write_accesses += PT_WAY;
      for (uint32_t way = 0; way < PT_WAY; way++)
        c_delta[set][way] >>= 1;
      c_sig[set] >>= 1;
    }

#ifdef SPP_SANITY_CHECK
    if (victim_way == PT_WAY) {
      std::cout << "[PT] Cannot find a replacement victim!" << std::endl;
      assert(0);
    }
#endif
  }
}

void PATTERN_TABLE::read_pattern(uint32_t curr_sig, int* delta_q, uint32_t* confidence_q, int32_t* perc_sum_q, uint32_t& lookahead_way, uint32_t& lookahead_conf,
                                 uint32_t& pf_q_tail, uint32_t& depth, uint64_t addr, uint64_t base_addr, uint64_t train_addr, uint64_t curr_ip,
                                 int32_t train_delta, uint32_t last_sig, uint32_t pq_occupancy, uint32_t pq_SIZE, uint32_t mshr_occupancy, uint32_t mshr_SIZE)
{
  // Update (sig, delta) correlation
  uint32_t set = get_hash(curr_sig) % PT_SET, local_conf = 0, pf_conf = 0, max_conf = 0;

  bool found_candidate = false;

  if (c_sig[set]) {
    for (uint32_t way = 0; way < PT_WAY; way++) {
      _parent->PT_read_accesses++;

      local_conf = (100 * c_delta[set][way]) / c_sig[set];
      pf_conf = depth ? (_parent->GHR.global_accuracy * c_delta[set][way] / c_sig[set] * lookahead_conf / 100) : local_conf;

      int32_t perc_sum = _parent->PERC.perc_predict(train_addr, curr_ip, _parent->GHR.ip_1, _parent->GHR.ip_2, _parent->GHR.ip_3, train_delta + delta[set][way],
                                                    last_sig, curr_sig, pf_conf, depth);
      bool do_pf = (perc_sum >= PERC_THRESHOLD_LO) ? 1 : 0;
      bool fill_l2 = (perc_sum >= PERC_THRESHOLD_HI) ? 1 : 0;

      if (fill_l2 && (mshr_occupancy >= mshr_SIZE || pq_occupancy >= pq_SIZE))
        continue;

      // Now checking against the L2C_MSHR_SIZE
      // Saving some slots in the internal PF queue by checking against do_pf
      if (pf_conf && do_pf && pf_q_tail < 100) {
        confidence_q[pf_q_tail] = pf_conf;
        delta_q[pf_q_tail] = delta[set][way];
        perc_sum_q[pf_q_tail] = perc_sum;

        // Lookahead path follows the most confident entry
        if (pf_conf > max_conf) {
          lookahead_way = way;
          max_conf = pf_conf;
        }
        pf_q_tail++;
        found_candidate = true;
      }

      // Recording Perc negatives
      if (pf_conf && pf_q_tail < _parent->L2C_MSHR_SIZE && (perc_sum < PERC_THRESHOLD_HI)) {
        uint64_t pf_addr = (base_addr & ~(static_cast<uint64_t>(BLOCK_SIZE) - 1)) + (delta[set][way] << LOG2_BLOCK_SIZE);

        if ((addr & ~(static_cast<uint64_t>(PAGE_SIZE) - 1)) == (pf_addr & ~(static_cast<uint64_t>(PAGE_SIZE) - 1))) { // Prefetch request is in the same physical page
          _parent->FILTER.check(pf_addr, train_addr, curr_ip, SPP_PERC_REJECT, train_delta + delta[set][way], last_sig, curr_sig, pf_conf, perc_sum, depth);
          _parent->GHR.perc_reject++;
        }
      }
    }
    lookahead_conf = max_conf;
    if (found_candidate)
      depth++;
  } else
    confidence_q[pf_q_tail] = 0;
}

bool PREFETCH_FILTER::check(uint64_t check_addr, uint64_t base_addr, uint64_t ip, FILTER_REQUEST filter_request, int cur_delta, uint32_t last_sig,
                           uint32_t curr_sig, uint32_t conf, int32_t sum, uint32_t depth)
{
  uint64_t cache_line = check_addr >> LOG2_BLOCK_SIZE, hash = get_hash(cache_line);

  // MAIN FILTER
  uint64_t quotient = (hash >> REMAINDER_BIT) & ((1 << QUOTIENT_BIT) - 1), remainder = hash % (1 << REMAINDER_BIT);

  // REJECT FILTER
  uint64_t quotient_reject = (hash >> REMAINDER_BIT_REJ) & ((1 << QUOTIENT_BIT_REJ) - 1), remainder_reject = hash % (1 << REMAINDER_BIT_REJ);

  switch (filter_request) {
  case SPP_PERC_REJECT: // To see what would have been the prediction given perceptron has rejected the PF
    _parent->FILTER_read_accesses++;
    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
      // We want to check if the prefetch would have gone through had perc not rejected.
      return false; // False return indicates "Do not prefetch"
    } else {
      if (train_neg) {
        _parent->FILTER_write_accesses++;
        _parent->GHR_ip_read_accesses += 3;

        valid_reject[quotient_reject] = 1;
        remainder_tag_reject[quotient_reject] = remainder_reject;

        // Logging perc features
        address_reject[quotient_reject] = base_addr;
        pc_reject[quotient_reject] = ip;
        pc_1_reject[quotient_reject] = _parent->GHR.ip_1;
        pc_2_reject[quotient_reject] = _parent->GHR.ip_2;
        pc_3_reject[quotient_reject] = _parent->GHR.ip_3;
        delta_reject[quotient_reject] = cur_delta;
        perc_sum_reject[quotient_reject] = sum;
        last_signature_reject[quotient_reject] = last_sig;
        cur_signature_reject[quotient_reject] = curr_sig;
        confidence_reject[quotient_reject] = conf;
        la_depth_reject[quotient_reject] = depth;
      }
    }
    break;

  case SPP_L2C_PREFETCH:
    _parent->FILTER_read_accesses++;

    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
      return false; // False return indicates "Do not prefetch"
    }
    break;

  case SPP_LLC_PREFETCH:
    _parent->FILTER_read_accesses++;
    if ((valid[quotient] || useful[quotient]) && remainder_tag[quotient] == remainder) {
      return false; // False return indicates "Do not prefetch"
    } else {
      // NOTE: SPP_LLC_PREFETCH has relatively low confidence
      // To allow this fast prefetch from LLC, SPP does not set the valid bit for SPP_LLC_PREFETCH
    }
    break;

  case L2C_DEMAND:
    _parent->FILTER_read_accesses++;
    if ((remainder_tag[quotient] == remainder) && (useful[quotient] == 0)) {
      _parent->FILTER_write_accesses++;
      useful[quotient] = 1;
      if (valid[quotient]) {
        _parent->GHR_counter_write_accesses++;
        _parent->GHR.pf_useful++; // This cache line was prefetched by SPP and actually used in the program
        _parent->GHR.pf_l2c_good++;
      }

      if (valid[quotient]) {
        // Prefetch leads to a demand hit
        _parent->PERC.perc_update(address[quotient], pc[quotient], pc_1[quotient], pc_2[quotient], pc_3[quotient], delta[quotient], last_signature[quotient],
                                  cur_signature[quotient], confidence[quotient], la_depth[quotient], 1, perc_sum[quotient]);

        // Histogramming Idea
        int32_t perc_sum_shifted = perc_sum[quotient] + (PERC_COUNTER_MAX + 1) * PERC_FEATURES;
        int32_t hist_index = perc_sum_shifted / 10;
        hist_hits[hist_index]++;
      }
    }
    // If NOT Prefetched
    if (!(valid[quotient] && remainder_tag[quotient] == remainder)) {
      // AND If Rejected by Perc
      _parent->FILTER_read_accesses++;
      if (valid_reject[quotient_reject] && remainder_tag_reject[quotient_reject] == remainder_reject) {
        if (train_neg) {
          // Not prefetched but could have been a good idea to prefetch
          _parent->PERC.perc_update(address_reject[quotient_reject], pc_reject[quotient_reject], pc_1_reject[quotient_reject], pc_2_reject[quotient_reject],
                                    pc_3_reject[quotient_reject], delta_reject[quotient_reject], last_signature_reject[quotient_reject],
                                    cur_signature_reject[quotient_reject], confidence_reject[quotient_reject], la_depth_reject[quotient_reject], 0,
                                    perc_sum_reject[quotient_reject]);
          _parent->FILTER_write_accesses++;
          valid_reject[quotient_reject] = 0;
          remainder_tag_reject[quotient_reject] = 0;
          _parent->GHR.reject_update++;
        }
      }
    }
    break;

  case L2C_EVICT:
    _parent->FILTER_read_accesses++;
    _parent->FILTER_write_accesses++;
    _parent->FILTER_write_accesses++;
    // Decrease global pf_useful counter when there is a useless prefetch (prefetched but not used)
    if (valid[quotient] && !useful[quotient]) {
      if (_parent->GHR.pf_useful) {
        _parent->GHR.pf_useful--;
        _parent->GHR_counter_write_accesses++;
      }

      // Prefetch leads to eviction
      _parent->PERC.perc_update(address[quotient], pc[quotient], pc_1[quotient], pc_2[quotient], pc_3[quotient], delta[quotient], last_signature[quotient],
                                cur_signature[quotient], confidence[quotient], la_depth[quotient], 0, perc_sum[quotient]);
    }
    // Reset filter entry
    valid[quotient] = 0;
    useful[quotient] = 0;
    remainder_tag[quotient] = 0;

    // Reset reject filter too
    valid_reject[quotient_reject] = 0;
    remainder_tag_reject[quotient_reject] = 0;

    break;

  default:
    std::cout << "[FILTER] Invalid filter request type: " << filter_request << std::endl;
    assert(0);
  }

  return true;
}

bool PREFETCH_FILTER::add_to_filter(uint64_t check_addr, uint64_t base_addr, uint64_t ip, FILTER_REQUEST filter_request, int cur_delta, uint32_t last_sig,
                                    uint32_t curr_sig, uint32_t conf, int32_t sum, uint32_t depth)
{
  uint64_t cache_line = check_addr >> LOG2_BLOCK_SIZE, hash = get_hash(cache_line);

  // MAIN FILTER
  uint64_t quotient = (hash >> REMAINDER_BIT) & ((1 << QUOTIENT_BIT) - 1), remainder = hash % (1 << REMAINDER_BIT);

  switch (filter_request) {
  case SPP_L2C_PREFETCH:
    _parent->FILTER_write_accesses++;
    valid[quotient] = 1;  // Mark as prefetched
    useful[quotient] = 0; // Reset useful bit
    remainder_tag[quotient] = remainder;

    // Logging perc features
    delta[quotient] = cur_delta;
    pc[quotient] = ip;
    _parent->GHR_ip_read_accesses += 3;
    pc_1[quotient] = _parent->GHR.ip_1;
    pc_2[quotient] = _parent->GHR.ip_2;
    pc_3[quotient] = _parent->GHR.ip_3;
    last_signature[quotient] = last_sig;
    cur_signature[quotient] = curr_sig;
    confidence[quotient] = conf;
    address[quotient] = base_addr;
    perc_sum[quotient] = sum;
    la_depth[quotient] = depth;
    break;

  case SPP_LLC_PREFETCH:
    // NOTE: To allow fast prefetch from LLC, SPP does not set the valid bit for SPP_LLC_PREFETCH
    break;
  default:
    std::cout << "[FILTER] Invalid filter request type: " << filter_request << std::endl;
    assert(0);
  }
  return true;
}

void GLOBAL_REGISTER::update_entry(uint32_t pf_sig, uint32_t pf_confidence, uint32_t pf_offset, int pf_delta)
{
  // NOTE: GHR implementation is slightly different from the original paper
  // Instead of matching (last_offset + delta), GHR simply stores and matches the pf_offset
  uint32_t min_conf = 100, victim_way = MAX_GHR_ENTRY;

  for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
    _parent->GHR_read_accesses++;

    // If GHR already holds the same pf_offset, update the GHR entry with the latest info
    if (valid[i] && (offset[i] == pf_offset)) {
      _parent->GHR_write_accesses++;
      sig[i] = pf_sig;
      confidence[i] = pf_confidence;
      delta[i] = pf_delta;

      return;
    }

    // GHR replacement policy is based on the stored confidence value
    if (confidence[i] < min_conf) {
      min_conf = confidence[i];
      victim_way = i;
    }
  }

  if (victim_way >= MAX_GHR_ENTRY) {
    std::cout << "[GHR] Cannot find a replacement victim!" << std::endl;
    assert(0);
  }

  _parent->GHR_write_accesses++;
  valid[victim_way] = 1;
  sig[victim_way] = pf_sig;
  confidence[victim_way] = pf_confidence;
  offset[victim_way] = pf_offset;
  delta[victim_way] = pf_delta;
}

uint32_t GLOBAL_REGISTER::check_entry(uint32_t page_offset)
{
  uint32_t max_conf = 0, max_conf_way = MAX_GHR_ENTRY;

  _parent->GHR_read_accesses += MAX_GHR_ENTRY;

  for (uint32_t i = 0; i < MAX_GHR_ENTRY; i++) {
    if ((offset[i] == page_offset) && (max_conf < confidence[i])) {
      max_conf = confidence[i];
      max_conf_way = i;
    }
  }

  return max_conf_way;
}

void PERCEPTRON::get_perc_index(uint64_t base_addr, uint64_t ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3, int32_t cur_delta, uint32_t last_sig,
                                uint32_t curr_sig, uint32_t confidence, uint32_t depth, uint64_t perc_set[PERC_FEATURES])
{
  // Returns the indexes for the perceptron tables
  uint64_t cache_line = base_addr >> LOG2_BLOCK_SIZE, page_addr = base_addr >> LOG2_PAGE_SIZE;

  int sig_delta = (cur_delta < 0) ? (((-1) * cur_delta) + (1 << (SIG_DELTA_BIT - 1))) : cur_delta;
  uint64_t pre_hash[PERC_FEATURES];

  pre_hash[0] = base_addr;
  pre_hash[1] = cache_line;
  pre_hash[2] = page_addr;
  pre_hash[3] = confidence ^ page_addr;
  pre_hash[4] = curr_sig ^ sig_delta;
  pre_hash[5] = ip_1 ^ (ip_2 >> 1) ^ (ip_3 >> 2);
  pre_hash[6] = ip ^ depth;
  pre_hash[7] = ip ^ sig_delta;
  pre_hash[8] = confidence;

  for (int i = 0; i < PERC_FEATURES; i++) {
    perc_set[i] = (pre_hash[i]) % PERC_DEPTH[i]; // Variable depths
  }
}

int32_t PERCEPTRON::perc_predict(uint64_t base_addr, uint64_t ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3, int32_t cur_delta, uint32_t last_sig,
                                 uint32_t curr_sig, uint32_t confidence, uint32_t depth)
{
  uint64_t perc_set[PERC_FEATURES];
  get_perc_index(base_addr, ip, ip_1, ip_2, ip_3, cur_delta, last_sig, curr_sig, confidence, depth, perc_set);

  _parent->PERC_read_accesses += PERC_FEATURES;

  int32_t sum = 0;
  for (int i = 0; i < PERC_FEATURES; i++) {
    sum += perc_weights[perc_set[i]][i]; // Calculate Sum
  }
  // Return the sum
  return sum;
}

void PERCEPTRON::perc_update(uint64_t base_addr, uint64_t ip, uint64_t ip_1, uint64_t ip_2, uint64_t ip_3, int32_t cur_delta, uint32_t last_sig,
                             uint32_t curr_sig, uint32_t confidence, uint32_t depth, bool direction, int32_t perc_sum)
{
  uint64_t perc_set[PERC_FEATURES];
  get_perc_index(base_addr, ip, ip_1, ip_2, ip_3, cur_delta, last_sig, curr_sig, confidence, depth, perc_set);

  _parent->PERC_write_accesses += PERC_FEATURES;

  int32_t sum = 0;
  for (int i = 0; i < PERC_FEATURES; i++) {
    // Marking the weights as touched for final dumping in the csv
    perc_touched[perc_set[i]][i] = 1;
  }
  // Restore the sum that led to the prediction
  sum = perc_sum;

  if (!direction) { // direction = 1 means the sum was in the correct direction, 0 means it was in the wrong direction
    // Prediction wrong
    for (int i = 0; i < PERC_FEATURES; i++) {
      if (sum >= PERC_THRESHOLD_HI) {
        // Prediction was to prefetch -- so decrement counters
        _parent->PERC_read_accesses++;
        if (perc_weights[perc_set[i]][i] > -1 * (PERC_COUNTER_MAX + 1)) {
          _parent->PERC_write_accesses++;
          perc_weights[perc_set[i]][i]--;
        }
      }
      if (sum < PERC_THRESHOLD_HI) {
        // Prediction was to not prefetch -- so increment counters
        _parent->PERC_read_accesses++;
        if (perc_weights[perc_set[i]][i] < PERC_COUNTER_MAX) {
          _parent->PERC_write_accesses++;
          perc_weights[perc_set[i]][i]++;
        }
      }
    }
  }
  if (direction && sum > NEG_UPDT_THRESHOLD && sum < POS_UPDT_THRESHOLD) {
    // Prediction correct but sum not 'saturated' enough
    for (int i = 0; i < PERC_FEATURES; i++) {
      if (sum >= PERC_THRESHOLD_HI) {
        // Prediction was to prefetch -- so increment counters
        _parent->PERC_read_accesses++;
        if (perc_weights[perc_set[i]][i] < PERC_COUNTER_MAX) {
          _parent->PERC_write_accesses++;
          perc_weights[perc_set[i]][i]++;
        }
      }
      if (sum < PERC_THRESHOLD_HI) {
        // Prediction was to not prefetch -- so decrement counters
        _parent->PERC_read_accesses++;
        if (perc_weights[perc_set[i]][i] > -1 * (PERC_COUNTER_MAX + 1)) {
          _parent->PERC_write_accesses++;
          perc_weights[perc_set[i]][i]--;
        }
      }
    }
  }
}

} // namespace spp_ppf_space

////////////////////////////////////////////////////////////
// spp_ppf prefetcher (class-based ChampSim module)
////////////////////////////////////////////////////////////

uint64_t spp_ppf::get_hash(uint64_t key) { return spp_ppf_space::get_hash(key); }

void spp_ppf::init_tables()
{
  ST._parent = this;
  PT._parent = this;
  FILTER._parent = this;
  GHR._parent = this;
  PERC._parent = this;
}

void spp_ppf::prefetcher_initialize()
{
  init_tables();

  std::cout << intern_->NAME << " SPP-PPF prefetcher" << std::endl;
  std::cout << "PERC_ENTRIES: " << PERC_ENTRIES << std::endl;

  for (int a = 0; a < 30; a++)
    depth_track[a] = 0;

  prefetch_q_full = 0;
}

void spp_ppf::prefetcher_cycle_operate() {}

std::vector<spp_ppf_space::PrefetchRequest> spp_ppf::generate(uint64_t addr, uint64_t ip, uint32_t pq_occupancy, uint32_t pq_size, uint32_t mshr_occupancy,
                                                              uint32_t mshr_size, bool warmup)
{
  std::vector<spp_ppf_space::PrefetchRequest> requests;

  // Neelu: Resetting counters after warmup complete
  if (warmup_flag_l2 == 0 && warmup) {
    ST_read_accesses = 0;
    ST_write_accesses = 0;
    PT_read_accesses = 0;
    PT_write_accesses = 0;
    FILTER_read_accesses = 0;
    FILTER_write_accesses = 0;
    GHR_read_accesses = 0;
    GHR_write_accesses = 0;
    PERC_read_accesses = 0;
    PERC_write_accesses = 0;
    GHR_counter_read_accesses = 0;
    GHR_counter_write_accesses = 0;
    GHR_ip_read_accesses = 0;
    GHR_ip_write_accesses = 0;
    ST_tag_read_accesses = 0;
    ST_tag_write_accesses = 0;
    warmup_flag_l2 = 1;
  }

  L2C_MSHR_SIZE = mshr_size;

  uint64_t page = addr >> LOG2_PAGE_SIZE;
  uint32_t page_offset = (addr >> LOG2_BLOCK_SIZE) & (PAGE_SIZE / BLOCK_SIZE - 1), last_sig = 0, curr_sig = 0, depth = 0;

  const std::size_t q_size = 100 * static_cast<std::size_t>(mshr_size) + 8;
  std::vector<uint32_t> confidence_q(q_size, 0);
  std::vector<int> delta_q(q_size, 0);
  std::vector<int32_t> perc_sum_q(q_size, 0);
  int32_t delta = 0;

  confidence_q[0] = 100;

  GHR_counter_write_accesses++;
  GHR_counter_read_accesses += 2; // pf_issued and pf_useful

  GHR.global_accuracy = GHR.pf_issued ? ((100 * GHR.pf_useful) / GHR.pf_issued) : 0;

  // Neelu: GHR Page tracker is simply for getting some stats
  for (int i = PAGES_TRACKED - 1; i > 0; i--) { // N down to 1
    GHR.page_tracker[i] = GHR.page_tracker[i - 1];
  }
  GHR.page_tracker[0] = page;

  int distinct_pages = 0;
  uint8_t num_pf = 0;
  for (int i = 0; i < PAGES_TRACKED; i++) {
    int j;
    for (j = 0; j < i; j++) {
      if (GHR.page_tracker[i] == GHR.page_tracker[j])
        break;
    }
    if (i == j)
      distinct_pages++;
  }

  // Stage 1: Read and update a sig stored in ST
  ST.read_and_update_sig(page, page_offset, last_sig, curr_sig, delta);

  FILTER.train_neg = 1;

  // Also check the prefetch filter in parallel to update global accuracy counters
  FILTER.check(addr, 0, 0, spp_ppf_space::L2C_DEMAND, 0, 0, 0, 0, 0, 0);

  // Stage 2: Update delta patterns stored in PT
  if (last_sig)
    PT.update_pattern(last_sig, delta);

  // Stage 3: Start prefetching
  uint64_t base_addr = addr;
  uint64_t curr_ip = ip;
  uint32_t lookahead_conf = 100, pf_q_head = 0, pf_q_tail = 0;
  uint8_t do_lookahead = 0;
  int32_t prev_delta = 0;

  uint64_t train_addr = addr;
  int32_t train_delta = 0;

  GHR_ip_read_accesses += 3;
  GHR_ip_write_accesses += 4;
  GHR.ip_3 = GHR.ip_2;
  GHR.ip_2 = GHR.ip_1;
  GHR.ip_1 = GHR.ip_0;
  GHR.ip_0 = ip;

#ifdef LOOKAHEAD_ON
  do {
#endif
    uint32_t lookahead_way = PT_WAY;

    train_addr = addr;
    train_delta = prev_delta;

    // Read the PT. Also passing info required for perceptron inferencing as PT calls perc_predict()
    PT.read_pattern(curr_sig, delta_q.data(), confidence_q.data(), perc_sum_q.data(), lookahead_way, lookahead_conf, pf_q_tail, depth, addr, base_addr,
                    train_addr, curr_ip, train_delta, last_sig, pq_occupancy, pq_size, mshr_occupancy, mshr_size);

    do_lookahead = 0;
    for (uint32_t i = pf_q_head; i < pf_q_tail; i++) {
      uint64_t pf_addr = (base_addr & ~(static_cast<uint64_t>(BLOCK_SIZE) - 1)) + (delta_q[i] << LOG2_BLOCK_SIZE);
      int32_t perc_sum = perc_sum_q[i];

      spp_ppf_space::FILTER_REQUEST fill_level = (perc_sum >= PERC_THRESHOLD_HI) ? spp_ppf_space::SPP_L2C_PREFETCH : spp_ppf_space::SPP_LLC_PREFETCH;

      if ((addr & ~(static_cast<uint64_t>(PAGE_SIZE) - 1)) == (pf_addr & ~(static_cast<uint64_t>(PAGE_SIZE) - 1))) { // Prefetch request is in the same physical page

        // Filter checks for redundancy and returns FALSE if redundant
        if (num_pf < ceil(((double)pq_size) / distinct_pages)) {
          if (FILTER.check(pf_addr, train_addr, curr_ip, fill_level, train_delta + delta_q[i], last_sig, curr_sig, confidence_q[i], perc_sum, (depth - 1))) {
            // Histogramming Idea
            int32_t perc_sum_shifted = perc_sum + (PERC_COUNTER_MAX + 1) * PERC_FEATURES;
            int32_t hist_index = perc_sum_shifted / 10;
            FILTER.hist_tots[hist_index]++;

            bool fill_this_level = (fill_level == spp_ppf_space::SPP_L2C_PREFETCH);
            uint32_t pf_metadata = fill_this_level ? 0x1 : 0x2;

            // Queue the candidate; the actual filter update is deferred to on_issue()
            spp_ppf_space::PrefetchRequest req;
            req.pf_addr = pf_addr;
            req.fill_this_level = fill_this_level;
            req.metadata = pf_metadata;
            req.train_addr = train_addr;
            req.curr_ip = curr_ip;
            req.fill_level = fill_level;
            req.delta = train_delta + delta_q[i];
            req.last_sig = last_sig;
            req.curr_sig = curr_sig;
            req.confidence = confidence_q[i];
            req.perc_sum = perc_sum;
            req.depth = depth - 1;
            requests.push_back(req);
            num_pf++;

            // Only for stats
            GHR.perc_pass++;
            GHR.depth_val = 1;
            GHR.pf_total++;
            if (fill_level == spp_ppf_space::SPP_L2C_PREFETCH)
              GHR.pf_l2c++;
            if (fill_level == spp_ppf_space::SPP_LLC_PREFETCH)
              GHR.pf_llc++;

            if (fill_level == spp_ppf_space::SPP_L2C_PREFETCH) {
              GHR_counter_write_accesses++;
              GHR.pf_issued++;
              if (GHR.pf_issued > GLOBAL_COUNTER_MAX) {
                GHR.pf_issued >>= 1;
                GHR_counter_write_accesses++;
                GHR.pf_useful >>= 1;
              }
            }
          }
        }
      } else { // Prefetch request is crossing the physical page boundary
#ifdef GHR_ON
        // Store this prefetch request in GHR to bootstrap SPP learning when we see a ST miss (i.e., accessing a new page)
        GHR.update_entry(curr_sig, confidence_q[i], (pf_addr >> LOG2_BLOCK_SIZE) & 0x3F, delta_q[i]);
#endif
      }
      do_lookahead = 1;
      pf_q_head++;
    }

    // Update base_addr and curr_sig
    if (lookahead_way < PT_WAY) {
      PT_read_accesses++;

      uint32_t set = get_hash(curr_sig) % PT_SET;
      base_addr += (PT.delta[set][lookahead_way] << LOG2_BLOCK_SIZE);
      prev_delta += PT.delta[set][lookahead_way];

      int sig_delta =
          (PT.delta[set][lookahead_way] < 0) ? (((-1) * PT.delta[set][lookahead_way]) + (1 << (SIG_DELTA_BIT - 1))) : PT.delta[set][lookahead_way];
      curr_sig = ((curr_sig << SIG_SHIFT) ^ sig_delta) & SIG_MASK;
    }
#ifdef LOOKAHEAD_ON
  } while (do_lookahead);
#endif

  // Stats
  if (GHR.depth_val) {
    GHR.depth_num++;
    GHR.depth_sum += depth;
  }

  if (depth < 30)
    depth_track[depth]++;

  return requests;
}

void spp_ppf::on_issue(const spp_ppf_space::PrefetchRequest& req)
{
  FILTER.add_to_filter(req.pf_addr, req.train_addr, req.curr_ip, req.fill_level, req.delta, req.last_sig, req.curr_sig, req.confidence, req.perc_sum, req.depth);
}

void spp_ppf::notify_fill_evict(uint64_t evicted_addr)
{
#ifdef FILTER_ON
  FILTER.check(evicted_addr, 0, 0, spp_ppf_space::L2C_EVICT, 0, 0, 0, 0, 0, 0);
#endif
}

uint32_t spp_ppf::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t /*cache_hit*/, bool /*useful_prefetch*/, access_type /*type*/,
                                           uint32_t metadata_in)
{
  auto requests = generate(addr.to<uint64_t>(), ip.to<uint64_t>(), static_cast<uint32_t>(intern_->get_pq_occupancy().back()),
                           static_cast<uint32_t>(intern_->get_pq_size().back()), static_cast<uint32_t>(intern_->get_mshr_occupancy()),
                           static_cast<uint32_t>(intern_->get_mshr_size()), intern_->warmup);

  for (const auto& req : requests) {
    bool issued = prefetch_line(champsim::address{req.pf_addr}, req.fill_this_level, req.metadata);
    if (issued)
      on_issue(req);
    else
      prefetch_q_full++;
  }

  return metadata_in;
}

uint32_t spp_ppf::prefetcher_cache_fill(champsim::address /*addr*/, long /*set*/, long /*way*/, uint8_t /*prefetch*/, champsim::address evicted_addr,
                                        uint32_t metadata_in)
{
  notify_fill_evict(evicted_addr.to<uint64_t>());
  return metadata_in;
}

void spp_ppf::prefetcher_final_stats()
{
  int tot = 0;
  printf("------------------\n");
  printf("Depth Distribution\n");
  printf("------------------\n");
  for (int a = 0; a < 30; a++) {
    printf("depth %d: %d\n", a, depth_track[a]);
    tot += depth_track[a];
  }
  printf("Total: %d\n", tot);
  printf("------------------\n");

  // Neelu: Printing Access Stats
  std::cout << "ST read accesses: " << ST_read_accesses << std::endl;
  std::cout << "ST write accesses: " << ST_write_accesses << std::endl;
  std::cout << "PT read accesses: " << PT_read_accesses << std::endl;
  std::cout << "PT write accesses: " << PT_write_accesses << std::endl;
  std::cout << "FILTER read accesses: " << FILTER_read_accesses << std::endl;
  std::cout << "FILTER write accesses: " << FILTER_write_accesses << std::endl;
  std::cout << "GHR read accesses: " << GHR_read_accesses << std::endl;
  std::cout << "GHR write accesses: " << GHR_write_accesses << std::endl;
  std::cout << "PERC read accesses: " << PERC_read_accesses << std::endl;
  std::cout << "PERC write accesses: " << PERC_write_accesses << std::endl;
  std::cout << "GHR counter read accesses: " << GHR_counter_read_accesses << std::endl;
  std::cout << "GHR counter write accesses: " << GHR_counter_write_accesses << std::endl;
  std::cout << "GHR ip read accesses: " << GHR_ip_read_accesses << std::endl;
  std::cout << "GHR ip write accesses: " << GHR_ip_write_accesses << std::endl;
  std::cout << "ST tag read accesses: " << ST_tag_read_accesses << std::endl;
  std::cout << "ST tag write accesses: " << ST_tag_write_accesses << std::endl;
}
