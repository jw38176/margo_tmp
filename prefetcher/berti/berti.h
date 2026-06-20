#ifndef PREFETCHER_BERTI_H
#define PREFETCHER_BERTI_H

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <queue>
#include <vector>

#include "access_type.h"
#include "address.h"
#include "berti_size.h"
#include "modules.h"

namespace berti_space
{

struct welford_t
{
  uint64_t num = 0;
  float average = 0.0F;
};

struct delta_t
{
  uint64_t conf = 0;
  int64_t delta = 0;
  uint8_t rpl = BERTI_R;
  float per = 0.0F;
};

class LatencyTable
{
public:
  explicit LatencyTable(int size);

  uint8_t add(uint64_t addr, uint64_t tag, bool pf, uint64_t cycle);
  uint64_t get(uint64_t addr);
  uint64_t del(uint64_t addr);
  uint64_t get_tag(uint64_t addr);

  ~LatencyTable();

private:
  struct latency_table
  {
    uint64_t addr = 0;
    uint64_t tag = 0;
    uint64_t time = 0;
    bool pf = false;
  };

  int size;
  latency_table* latencyt;
};

class ShadowCache
{
public:
  ShadowCache(int sets, int ways);
  ~ShadowCache();

  bool add(uint32_t set, uint32_t way, uint64_t addr, bool pf, uint64_t lat);
  bool get(uint64_t addr);
  void set_pf(uint64_t addr, bool pf);
  bool is_pf(uint64_t addr);
  uint64_t get_latency(uint64_t addr);

private:
  struct shadow_cache
  {
    uint64_t addr = 0;
    uint64_t lat = 0;
    bool pf = false;
  };

  int sets;
  int ways;
  shadow_cache** scache;
};

class HistoryTable
{
public:
  HistoryTable();
  ~HistoryTable();

  void add(uint64_t tag, uint64_t addr, uint64_t cycle);
  uint16_t get(uint32_t latency, uint64_t tag, uint64_t act_addr, uint64_t* tags, uint64_t* addr, uint64_t cycle);

private:
  struct history_table
  {
    uint64_t tag = 0;
    uint64_t addr = 0;
    uint64_t time = 0;
  };

  history_table** historyt;
  history_table** history_pointers;

  uint16_t get_aux(uint32_t latency, uint64_t tag, uint64_t act_addr, uint64_t* tags, uint64_t* addr, uint64_t cycle);
};

class BertiTable
{
public:
  BertiTable(uint64_t p_size, HistoryTable* historyt);
  ~BertiTable();

  void find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle, uint64_t line_addr);
  uint8_t get(uint64_t tag, delta_t res[MAX_PF]);

private:
  struct berti_entry
  {
    std::array<delta_t, BERTI_TABLE_DELTA_SIZE> deltas{};
    uint64_t conf = 0;
    uint64_t total_used = 0;
  };

  HistoryTable* historyt;
  std::map<uint64_t, berti_entry*> bertit;
  std::queue<uint64_t> bertit_queue;
  uint64_t size = 0;

  static bool compare_greater_delta(delta_t a, delta_t b);
  static bool compare_greater_delta_per(delta_t a, delta_t b);

  void increase_conf_tag(uint64_t tag);
  void add(uint64_t tag, int64_t delta);
};

} // namespace berti_space

class berti : public champsim::modules::prefetcher
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
  std::unique_ptr<berti_space::LatencyTable> latencyt;
  std::unique_ptr<berti_space::ShadowCache> scache;
  std::unique_ptr<berti_space::HistoryTable> historyt;
  std::unique_ptr<berti_space::BertiTable> berti_table;

  berti_space::welford_t average_latency{};
  uint64_t pf_to_l1 = 0;
  uint64_t pf_to_l2 = 0;
  uint64_t pf_to_l2_bc_mshr = 0;
  uint64_t cant_track_latency = 0;
  uint64_t cross_page = 0;
  uint64_t no_cross_page = 0;
  uint64_t average_issued = 0;
  uint64_t average_num = 0;

  bool _collect_candidates = false;
  std::vector<Candidate> _candidates;

  uint64_t current_cycle() const;
  void submit_prefetch(uint64_t pf_addr, uint64_t line_addr, uint64_t ip_hash, bool fill_this_level, uint32_t metadata, champsim::address trigger_ip);
};

#endif
