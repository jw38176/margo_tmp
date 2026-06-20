#include "berti.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include "cache.h"
#include "champsim.h"

using namespace berti_space;

namespace
{

uint64_t ip_hash(uint64_t ip) { return ((ip >> 1) ^ (ip >> 4)) & IP_MASK; }

} // namespace

LatencyTable::LatencyTable(int table_size) : size(table_size) { latencyt = new latency_table[size](); }

LatencyTable::~LatencyTable() { delete[] latencyt; }

uint8_t LatencyTable::add(uint64_t addr, uint64_t tag, bool pf, uint64_t cycle)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr << " tag: " << tag;
    std::cout << " prefetch: " << std::dec << +pf << " cycle: " << cycle;
  }

  latency_table* free = nullptr;

  for (int i = 0; i < size; i++) {
    if (latencyt[i].addr == addr) {
      if constexpr (champsim::debug_print) {
        std::cout << " line already found; find_tag: " << latencyt[i].tag;
        std::cout << " find_pf: " << +latencyt[i].pf << std::endl;
      }
      latencyt[i].time = cycle;
      latencyt[i].tag = tag;
      latencyt[i].pf = pf;
      return latencyt[i].pf;
    }

    if (latencyt[i].tag == 0) {
      free = &latencyt[i];
    }
  }

  if (free == nullptr) {
    return 0;
  }

  free->addr = addr;
  free->time = cycle;
  free->tag = tag;
  free->pf = pf;

  if constexpr (champsim::debug_print) {
    std::cout << " new entry" << std::endl;
  }
  return free->pf;
}

uint64_t LatencyTable::del(uint64_t addr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr;
  }

  for (int i = 0; i < size; i++) {
    if (latencyt[i].addr == addr) {
      uint64_t time = latencyt[i].time;

      if constexpr (champsim::debug_print) {
        std::cout << " tag: " << latencyt[i].tag;
        std::cout << " prefetch: " << std::dec << +latencyt[i].pf;
        std::cout << " cycle: " << latencyt[i].time << std::endl;
      }

      latencyt[i].addr = 0;
      latencyt[i].tag = 0;
      latencyt[i].time = 0;
      latencyt[i].pf = 0;

      return time;
    }
  }

  if constexpr (champsim::debug_print) {
    std::cout << " TRANSLATION" << std::endl;
  }
  return 0;
}

uint64_t LatencyTable::get(uint64_t addr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < size; i++) {
    if (latencyt[i].addr == addr) {
      if constexpr (champsim::debug_print) {
        std::cout << " time: " << latencyt[i].time << std::endl;
      }
      return latencyt[i].time;
    }
  }

  if constexpr (champsim::debug_print) {
    std::cout << " NOT FOUND" << std::endl;
  }
  return 0;
}

uint64_t LatencyTable::get_tag(uint64_t addr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_LATENCY_TABLE] " << __func__;
    std::cout << " addr: " << std::hex << addr;
  }

  for (int i = 0; i < size; i++) {
    if (latencyt[i].addr == addr && latencyt[i].tag) {
      if constexpr (champsim::debug_print) {
        std::cout << " tag: " << latencyt[i].tag << std::endl;
      }
      return latencyt[i].tag;
    }
  }

  if constexpr (champsim::debug_print) {
    std::cout << " NOT_FOUND" << std::endl;
  }
  return 0;
}

ShadowCache::ShadowCache(int num_sets, int num_ways) : sets(num_sets), ways(num_ways)
{
  scache = new shadow_cache*[sets];
  for (int i = 0; i < sets; i++) {
    scache[i] = new shadow_cache[ways]();
  }
}

ShadowCache::~ShadowCache()
{
  for (int i = 0; i < sets; i++) {
    delete[] scache[i];
  }
  delete[] scache;
}

bool ShadowCache::add(uint32_t set, uint32_t way, uint64_t addr, bool pf, uint64_t lat)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " set: " << set << " way: " << way;
    std::cout << " addr: " << std::hex << addr << std::dec;
    std::cout << " pf: " << +pf;
    std::cout << " latency: " << lat << std::endl;
  }

  scache[set][way].addr = addr;
  scache[set][way].pf = pf;
  scache[set][way].lat = lat;
  return scache[set][way].pf;
}

bool ShadowCache::get(uint64_t addr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::endl;
  }

  for (int i = 0; i < sets; i++) {
    for (int ii = 0; ii < ways; ii++) {
      if (scache[i][ii].addr == addr) {
        return true;
      }
    }
  }

  return false;
}

void ShadowCache::set_pf(uint64_t addr, bool pf)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < sets; i++) {
    for (int ii = 0; ii < ways; ii++) {
      if (scache[i][ii].addr == addr) {
        if constexpr (champsim::debug_print) {
          std::cout << " set: " << i << " way: " << ii;
          std::cout << " old_pf_value: " << +scache[i][ii].pf;
          std::cout << " new_pf_value: " << +pf << std::endl;
        }
        scache[i][ii].pf = pf;
        return;
      }
    }
  }

  assert((0) && "Address must be in shadow cache");
}

bool ShadowCache::is_pf(uint64_t addr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < sets; i++) {
    for (int ii = 0; ii < ways; ii++) {
      if (scache[i][ii].addr == addr) {
        if constexpr (champsim::debug_print) {
          std::cout << " set: " << i << " way: " << ii;
          std::cout << " pf: " << +scache[i][ii].pf << std::endl;
        }
        return scache[i][ii].pf;
      }
    }
  }

  assert((0) && "Address must be in shadow cache");
  return false;
}

uint64_t ShadowCache::get_latency(uint64_t addr)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_SHADOW_CACHE] " << __func__;
    std::cout << " addr: " << std::hex << addr << std::dec;
  }

  for (int i = 0; i < sets; i++) {
    for (int ii = 0; ii < ways; ii++) {
      if (scache[i][ii].addr == addr) {
        if constexpr (champsim::debug_print) {
          std::cout << " set: " << i << " way: " << ii;
          std::cout << " latency: " << scache[i][ii].lat << std::endl;
        }
        return scache[i][ii].lat;
      }
    }
  }

  assert((0) && "Address must be in shadow cache");
  return 0;
}

HistoryTable::HistoryTable()
{
  history_pointers = new history_table*[HISTORY_TABLE_SETS];
  historyt = new history_table*[HISTORY_TABLE_SETS];

  for (int i = 0; i < HISTORY_TABLE_SETS; i++) {
    historyt[i] = new history_table[HISTORY_TABLE_WAYS]();
  }
  for (int i = 0; i < HISTORY_TABLE_SETS; i++) {
    history_pointers[i] = historyt[i];
  }
}

HistoryTable::~HistoryTable()
{
  for (int i = 0; i < HISTORY_TABLE_SETS; i++) {
    delete[] historyt[i];
  }
  delete[] historyt;
  delete[] history_pointers;
}

void HistoryTable::add(uint64_t tag, uint64_t addr, uint64_t cycle)
{
  uint16_t set = tag & TABLE_SET_MASK;

  if (history_pointers[set] == &historyt[set][HISTORY_TABLE_WAYS - 1]) {
    if (historyt[set][0].addr == (addr & ADDR_MASK)) {
      return;
    }
  } else if ((history_pointers[set] - 1)->addr == (addr & ADDR_MASK)) {
    return;
  }

  history_pointers[set]->tag = tag;
  history_pointers[set]->time = cycle & TIME_MASK;
  history_pointers[set]->addr = addr & ADDR_MASK;

  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_HISTORY_TABLE] " << __func__;
    std::cout << " tag: " << std::hex << tag << " line_addr: " << addr << std::dec;
    std::cout << " cycle: " << cycle << " set: " << set << std::endl;
  }

  if (history_pointers[set] == &historyt[set][HISTORY_TABLE_WAYS - 1]) {
    history_pointers[set] = &historyt[set][0];
  } else {
    history_pointers[set]++;
  }
}

uint16_t HistoryTable::get_aux(uint32_t latency, uint64_t tag, uint64_t act_addr, uint64_t* tags, uint64_t* addr, uint64_t cycle)
{
  uint16_t num_on_time = 0;
  uint16_t set = tag & TABLE_SET_MASK;

  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_HISTORY_TABLE] " << __func__;
    std::cout << " tag: " << std::hex << tag << std::dec;
    std::cout << " cycle: " << cycle << " set: " << set << std::endl;
  }

  if (cycle < latency) {
    return num_on_time;
  }

  cycle -= latency;

  history_table* pointer = history_pointers[set];

  do {
    if (pointer->tag == tag && pointer->time <= cycle) {
      if (pointer->addr == act_addr) {
        return num_on_time;
      }

      tags[num_on_time] = pointer->tag;
      addr[num_on_time] = pointer->addr;
      num_on_time++;
    }

    if (pointer == historyt[set]) {
      pointer = &historyt[set][HISTORY_TABLE_WAYS - 1];
    } else {
      pointer--;
    }
  } while (pointer != history_pointers[set]);

  return num_on_time;
}

uint16_t HistoryTable::get(uint32_t latency, uint64_t tag, uint64_t act_addr, uint64_t* tags, uint64_t* addr, uint64_t cycle)
{
  act_addr &= ADDR_MASK;
  return get_aux(latency, tag, act_addr, tags, addr, cycle & TIME_MASK);
}

BertiTable::BertiTable(uint64_t p_size, HistoryTable* historyt_arg) : historyt(historyt_arg), size(p_size) {}

BertiTable::~BertiTable()
{
  for (auto& kv : bertit) {
    delete kv.second;
  }
}

void BertiTable::increase_conf_tag(uint64_t tag)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_BERTI] " << __func__ << " tag: " << std::hex << tag << std::dec;
  }

  if (bertit.find(tag) == bertit.end()) {
    if constexpr (champsim::debug_print) {
      std::cout << " TAG NOT FOUND" << std::endl;
    }
    return;
  }

  bertit[tag]->conf += CONFIDENCE_INC;

  if constexpr (champsim::debug_print) {
    std::cout << " global_conf: " << bertit[tag]->conf;
  }

  if (bertit[tag]->conf == CONFIDENCE_MAX) {
    for (auto& i : bertit[tag]->deltas) {
      float temp = static_cast<float>(i.conf) / static_cast<float>(bertit[tag]->conf);
      uint64_t aux_conf = static_cast<uint64_t>(temp * 100);

      if (aux_conf > CONFIDENCE_L1) {
        i.rpl = BERTI_L1;
      } else if (aux_conf > CONFIDENCE_L2) {
        i.rpl = BERTI_L2;
      } else if (aux_conf > CONFIDENCE_L2R) {
        i.rpl = BERTI_L2R;
      } else {
        i.rpl = BERTI_R;
      }

      if constexpr (champsim::debug_print) {
        std::cout << "Delta: " << i.delta;
        std::cout << " Conf: " << i.conf << " Level: " << +i.rpl;
        std::cout << "|";
      }

      i.conf = 0;
    }

    bertit[tag]->conf = 0;
  }

  if constexpr (champsim::debug_print) {
    std::cout << std::endl;
  }
}

void BertiTable::add(uint64_t tag, int64_t delta)
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_BERTI] " << __func__;
    std::cout << " tag: " << std::hex << tag << std::dec;
    std::cout << " delta: " << delta;
  }

  if (bertit.find(tag) == bertit.end()) {
    if constexpr (champsim::debug_print) {
      std::cout << " allocating a new entry;";
    }

    if (bertit_queue.size() > BERTI_TABLE_SIZE) {
      uint64_t key = bertit_queue.front();
      berti_entry* entry = bertit[key];

      if constexpr (champsim::debug_print) {
        std::cout << " removing tag: " << std::hex << key << std::dec << ";";
      }

      delete entry;
      bertit.erase(bertit_queue.front());
      bertit_queue.pop();
    }

    bertit_queue.push(tag);
    assert((bertit.size() <= BERTI_TABLE_SIZE) && "Tracking too many tags");

    berti_entry* entry = new berti_entry;
    entry->conf = CONFIDENCE_INC;

    entry->deltas[0].delta = delta;
    entry->deltas[0].conf = CONFIDENCE_INIT;
    entry->deltas[0].rpl = BERTI_R;

    if constexpr (champsim::debug_print) {
      std::cout << " confidence: " << CONFIDENCE_INIT << std::endl;
    }

    bertit.insert(std::make_pair(tag, entry));
    return;
  }

  berti_entry* entry = bertit[tag];

  for (auto& i : entry->deltas) {
    if (i.delta == delta) {
      i.conf += CONFIDENCE_INC;
      if (i.conf > CONFIDENCE_MAX) {
        i.conf = CONFIDENCE_MAX;
      }

      if constexpr (champsim::debug_print) {
        std::cout << " confidence: " << i.conf << std::endl;
      }
      return;
    }
  }

  uint8_t dx_conf = 100;
  int dx_remove = -1;
  for (std::size_t i = 0; i < entry->deltas.size(); i++) {
    if (entry->deltas[i].delta == 0) {
      entry->deltas[i].delta = delta;
      entry->deltas[i].conf = CONFIDENCE_INIT;
      entry->deltas[i].rpl = BERTI_R;
      return;
    }
    if (entry->deltas[i].rpl == BERTI_R && entry->deltas[i].conf < dx_conf) {
      dx_conf = entry->deltas[i].conf;
      dx_remove = static_cast<int>(i);
    }
  }

  if (dx_remove > -1) {
    entry->deltas[dx_remove].delta = delta;
    entry->deltas[dx_remove].conf = CONFIDENCE_INIT;
    entry->deltas[dx_remove].rpl = BERTI_R;
    return;
  }

  dx_conf = 100;
  dx_remove = -1;
  for (std::size_t i = 0; i < entry->deltas.size(); i++) {
    if (entry->deltas[i].rpl == BERTI_L2R && entry->deltas[i].conf < dx_conf) {
      dx_conf = entry->deltas[i].conf;
      dx_remove = static_cast<int>(i);
    }
  }

  if (dx_remove > -1) {
    entry->deltas[dx_remove].delta = delta;
    entry->deltas[dx_remove].conf = CONFIDENCE_INIT;
    entry->deltas[dx_remove].rpl = BERTI_R;
  }
}

uint8_t BertiTable::get(uint64_t tag, delta_t res[MAX_PF])
{
  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI_BERTI] " << __func__ << " tag: " << std::hex << tag;
    std::cout << std::dec;
  }

  if (!bertit.count(tag)) {
    if constexpr (champsim::debug_print) {
      std::cout << " TAG NOT FOUND" << std::endl;
    }
    return 0;
  }

  if constexpr (champsim::debug_print) {
    std::cout << std::endl;
  }

  berti_entry* entry = bertit[tag];
  uint16_t dx = 0;

  for (auto& i : entry->deltas) {
    if (i.delta != 0 && i.rpl != BERTI_R) {
      res[dx].delta = i.delta;
      res[dx].rpl = i.rpl;
      dx++;
    }
  }

  if (dx == 0 && entry->conf >= 8) {
    for (auto& i : entry->deltas) {
      if (i.delta != 0) {
        res[dx].delta = i.delta;
        float temp = static_cast<float>(i.conf) / static_cast<float>(entry->conf);
        uint64_t aux_conf = static_cast<uint64_t>(temp * 100);
        res[dx].per = static_cast<float>(aux_conf);
        dx++;
      }
    }
    std::sort(res, res + MAX_PF, compare_greater_delta_per);

    for (int i = 0; i < MAX_PF; i++) {
      if (res[i].per > 80) {
        res[i].rpl = BERTI_L1;
      } else if (res[i].per > 35) {
        res[i].rpl = BERTI_L2;
      } else {
        res[i].rpl = BERTI_R;
      }
    }
    std::sort(res, res + MAX_PF, compare_greater_delta);
    return 1;
  }

  std::sort(res, res + MAX_PF, compare_greater_delta);
  return 1;
}

void BertiTable::find_and_update(uint64_t latency, uint64_t tag, uint64_t cycle, uint64_t line_addr)
{
  uint64_t tags[HISTORY_TABLE_WAYS];
  uint64_t addr[HISTORY_TABLE_WAYS];

  uint16_t num_on_time = historyt->get(latency, tag, line_addr, tags, addr, cycle);

  for (uint32_t i = 0; i < num_on_time; i++) {
    if (i == 0) {
      increase_conf_tag(tag);
    }

    if (i >= MAX_HISTORY_IP) {
      break;
    }

    line_addr &= ADDR_MASK;
    int64_t stride = static_cast<int64_t>(line_addr - addr[i]);

    if ((std::abs(stride) < (1 << DELTA_MASK))) {
      add(tags[i], stride);
    }
  }
}

bool BertiTable::compare_greater_delta(delta_t a, delta_t b)
{
  if (a.rpl == BERTI_L1 && b.rpl != BERTI_L1) {
    return 1;
  }
  if (a.rpl != BERTI_L1 && b.rpl == BERTI_L1) {
    return 0;
  }
  if (a.rpl == BERTI_L2 && b.rpl != BERTI_L2) {
    return 1;
  }
  if (a.rpl != BERTI_L2 && b.rpl == BERTI_L2) {
    return 0;
  }
  if (a.rpl == BERTI_L2R && b.rpl != BERTI_L2R) {
    return 1;
  }
  if (a.rpl != BERTI_L2R && b.rpl == BERTI_L2R) {
    return 0;
  }
  if (std::abs(a.delta) < std::abs(b.delta)) {
    return 1;
  }
  return 0;
}

bool BertiTable::compare_greater_delta_per(delta_t a, delta_t b)
{
  if (a.per > b.per) {
    return 1;
  }
  if (std::abs(a.delta) < std::abs(b.delta)) {
    return 1;
  }
  return 0;
}

uint64_t berti::current_cycle() const { return static_cast<uint64_t>(intern_->current_time.time_since_epoch() / intern_->clock_period); }

void berti::prefetcher_initialize()
{
  uint64_t latency_table_size = intern_->get_mshr_size();
  for (auto const& i : intern_->get_rq_size()) {
    latency_table_size += i;
  }
  for (auto const& i : intern_->get_wq_size()) {
    latency_table_size += i;
  }
  for (auto const& i : intern_->get_pq_size()) {
    latency_table_size += i;
  }

  historyt = std::make_unique<HistoryTable>();
  latencyt = std::make_unique<LatencyTable>(static_cast<int>(latency_table_size));
  scache = std::make_unique<ShadowCache>(intern_->NUM_SET, intern_->NUM_WAY);
  berti_table = std::make_unique<BertiTable>(BERTI_TABLE_DELTA_SIZE, historyt.get());

  std::cout << "Berti Prefetcher" << std::endl;
  std::cout << "History Sets: " << HISTORY_TABLE_SETS << std::endl;
  std::cout << "History Ways: " << HISTORY_TABLE_WAYS << std::endl;
  std::cout << "BERTI Size: " << BERTI_TABLE_SIZE << std::endl;
  std::cout << "BERTI Delta Size: " << BERTI_TABLE_DELTA_SIZE << std::endl;
}

void berti::submit_prefetch(uint64_t pf_addr, uint64_t line_addr, uint64_t ip_hash_val, bool fill_this_level, uint32_t metadata,
                            champsim::address trigger_ip)
{
  const uint64_t p_b_addr = pf_addr >> LOG2_BLOCK_SIZE;

  if (_collect_candidates) {
    _candidates.push_back({champsim::address{pf_addr}, trigger_ip, fill_this_level, metadata,
                           [this, p_b_addr, ip_hash_val, fill_this_level](bool issued, bool /*filtered_out*/) {
                             if (issued && fill_this_level && !scache->get(p_b_addr)) {
                               latencyt->add(p_b_addr, ip_hash_val, true, current_cycle());
                             }
                           }});
    return;
  }

  if (prefetch_line(champsim::address{pf_addr}, fill_this_level, metadata)) {
    average_issued++;
    if (fill_this_level && !scache->get(p_b_addr)) {
      latencyt->add(p_b_addr, ip_hash_val, true, current_cycle());
    }
  }
}

uint32_t berti::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                         uint32_t metadata_in)
{
  if ((type != access_type::LOAD) && (type != access_type::PREFETCH)) {
    return metadata_in;
  }

  const uint64_t raw_addr = addr.to<uint64_t>();
  const uint64_t raw_ip = ip.to<uint64_t>();
  const uint64_t line_addr = raw_addr >> LOG2_BLOCK_SIZE;

  if (line_addr == 0) {
    return metadata_in;
  }

  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI] operate";
    std::cout << " ip: " << std::hex << raw_ip;
    std::cout << " full_address: " << raw_addr;
    std::cout << " line_address: " << line_addr << std::dec << std::endl;
  }

  const uint64_t ip_hash_val = ip_hash(raw_ip);
  const uint64_t cycle = current_cycle();

  if (!cache_hit) {
    if constexpr (champsim::debug_print) {
      std::cout << "[BERTI] operate cache miss" << std::endl;
    }

    latencyt->add(line_addr, ip_hash_val, false, cycle);
    historyt->add(ip_hash_val, line_addr, cycle);
  } else if (cache_hit && scache->is_pf(line_addr)) {
    if constexpr (champsim::debug_print) {
      std::cout << "[BERTI] operate cache hit because of pf" << std::endl;
    }

    scache->set_pf(line_addr, false);

    uint64_t latency = scache->get_latency(line_addr);
    if (latency > LAT_MASK) {
      latency = 0;
    }

    berti_table->find_and_update(latency, ip_hash_val, cycle & TIME_MASK, line_addr);
    historyt->add(ip_hash_val, line_addr, cycle & TIME_MASK);
  } else if constexpr (champsim::debug_print) {
    std::cout << "[BERTI] operate cache hit" << std::endl;
  }

  delta_t deltas[MAX_PF]{};
  if (!berti_table->get(ip_hash_val, deltas)) {
    return metadata_in;
  }

  bool first_issue = true;
  for (int i = 0; i < MAX_PF_LAUNCH; i++) {
    if (deltas[i].delta == 0) {
      continue;
    }

    const uint64_t p_addr = (line_addr + deltas[i].delta) << LOG2_BLOCK_SIZE;
    const uint64_t p_b_addr = p_addr >> LOG2_BLOCK_SIZE;

    if (latencyt->get(p_b_addr)) {
      continue;
    }
    if (deltas[i].rpl == BERTI_R) {
      return metadata_in;
    }
    if (p_addr == 0) {
      continue;
    }

    if ((p_addr >> LOG2_PAGE_SIZE) != (raw_addr >> LOG2_PAGE_SIZE)) {
      cross_page++;
    } else {
      no_cross_page++;
    }

    const float mshr_load = static_cast<float>(intern_->get_mshr_occupancy_ratio() * 100.0);
    const bool fill_this_level = (deltas[i].rpl == BERTI_L1) && (mshr_load < MSHR_LIMIT);

    if (deltas[i].rpl == BERTI_L1 && mshr_load >= MSHR_LIMIT) {
      pf_to_l2_bc_mshr++;
    }
    if (fill_this_level) {
      pf_to_l1++;
    } else {
      pf_to_l2++;
    }

    if (_collect_candidates) {
      submit_prefetch(p_addr, line_addr, ip_hash_val, fill_this_level, 0x1, ip);
      if (first_issue) {
        first_issue = false;
        average_num++;
      }
      continue;
    }

    if (prefetch_line(champsim::address{p_addr}, fill_this_level, 0x1)) {
      average_issued++;
      if (first_issue) {
        first_issue = false;
        average_num++;
      }

      if constexpr (champsim::debug_print) {
        std::cout << "[BERTI] operate prefetch delta: " << deltas[i].delta;
        std::cout << " p_addr: " << std::hex << p_addr << std::dec;
        std::cout << " this_level: " << +fill_this_level << std::endl;
      }

      if (fill_this_level && !scache->get(p_b_addr)) {
        latencyt->add(p_b_addr, ip_hash_val, true, cycle);
      }
    }
  }

  return metadata_in;
}

uint32_t berti::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address /*evicted_addr*/,
                                      uint32_t metadata_in)
{
  const uint64_t line_addr = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
  const uint64_t tag = latencyt->get_tag(line_addr);
  const uint64_t cycle = latencyt->del(line_addr) & TIME_MASK;
  uint64_t latency = 0;

  if constexpr (champsim::debug_print) {
    std::cout << "[BERTI] fill addr: " << std::hex << line_addr;
    std::cout << " event_cycle: " << cycle;
    std::cout << " prefetch: " << +prefetch << std::endl;
    std::cout << " latency: " << latency << std::endl;
  }

  const uint64_t now = current_cycle() & TIME_MASK;
  if (cycle != 0 && now > cycle) {
    latency = now - cycle;
  }

  if (latency > LAT_MASK) {
    latency = 0;
    cant_track_latency++;
  } else if (latency != 0) {
    if (average_latency.num == 0) {
      average_latency.average = static_cast<float>(latency);
    } else {
      average_latency.average =
          average_latency.average + ((static_cast<float>(latency) - average_latency.average) / static_cast<float>(average_latency.num));
    }
    average_latency.num++;
  }

  scache->add(static_cast<uint32_t>(set), static_cast<uint32_t>(way), line_addr, prefetch != 0, latency);

  if (latency != 0 && prefetch == 0) {
    berti_table->find_and_update(latency, tag, cycle, line_addr);
  }

  return metadata_in;
}

void berti::prefetcher_final_stats()
{
  std::cout << "BERTI TO_L1: " << pf_to_l1 << " TO_L2: " << pf_to_l2;
  std::cout << " TO_L2_BC_MSHR: " << pf_to_l2_bc_mshr << std::endl;

  std::cout << "BERTI AVG_LAT: ";
  std::cout << average_latency.average << " NUM_TRACK_LATENCY: ";
  std::cout << average_latency.num << " NUM_CANT_TRACK_LATENCY: ";
  std::cout << cant_track_latency << std::endl;

  std::cout << "BERTI CROSS_PAGE " << cross_page;
  std::cout << " NO_CROSS_PAGE: " << no_cross_page << std::endl;

  std::cout << "BERTI AVERAGE_ISSUED: ";
  if (average_num != 0) {
    std::cout << ((1.0 * average_issued) / average_num);
  } else {
    std::cout << 0;
  }
  std::cout << std::endl;
}

std::vector<berti::Candidate> berti::prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                         access_type type, uint32_t metadata_in)
{
  _collect_candidates = true;
  _candidates.clear();
  prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
  _collect_candidates = false;
  return std::move(_candidates);
}
