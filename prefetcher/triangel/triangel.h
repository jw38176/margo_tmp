#pragma once

#include <vector>
#include <unordered_map>
#include <iostream>
#include <set>
#include <random>
#include <algorithm>
#include <limits>
#include <cassert>
#include <utility>
#include <functional>

#include "address.h"
#include "champsim.h"
#include "modules.h"
#include "cache.h"

using namespace std;

struct train_table_entry{
    uint32_t PC_tag;
    uint64_t last_addr0;
    uint64_t last_addr1;
    uint64_t timestamp;
    uint64_t local_timestamp;
    uint32_t reuse_conf;
    uint32_t pattern_conf0;
    uint32_t pattern_conf1;
    uint32_t sample_rate;
    bool lookahead;
    bool currently_twodist_pf;

    train_table_entry(){
        PC_tag = 0;
        last_addr0 = 0;
        last_addr1 = 0;
        timestamp = 0;
        local_timestamp = 0;
        reuse_conf = 7;
        pattern_conf0 = 7;
        pattern_conf1 = 7;
        sample_rate = 8;
        lookahead = 0;
        currently_twodist_pf = false;
    }

    train_table_entry(uint32_t PC, uint64_t last_addr, uint64_t time):PC_tag(PC),last_addr1(last_addr),timestamp(time){
      last_addr0 = 0;
      local_timestamp = 0;
      reuse_conf = 7;
      pattern_conf0 = 7;
      pattern_conf1 = 7;
      sample_rate = 8;
      lookahead = 0;
      currently_twodist_pf = false;
    }
};

struct metadata
{
    uint64_t tag;
    uint64_t target_addr;
    uint8_t conf;

    metadata() {
        tag = 0;
        target_addr = 0;
        conf = 0;
    }

    metadata(uint64_t tag, uint64_t target_addr):tag(tag), target_addr(target_addr), conf(0){}
};

struct sampler_entry
{
    uint32_t addr_tag;
    uint64_t train_pc;
    uint64_t target_addr;
    uint64_t timestamp;
    bool hit;

    sampler_entry(){
        addr_tag = 0;
        train_pc = 0;
        target_addr = 0;
        timestamp = 0;
        hit = 0;
    }

    sampler_entry(uint32_t addr_tag, uint64_t train_pc, uint64_t target_addr, uint64_t timestamp)
        :addr_tag(addr_tag), train_pc(train_pc), target_addr(target_addr), timestamp(timestamp), hit(0) {
    }
    bool operator==(const sampler_entry& other) const {
        return addr_tag == other.addr_tag
            && train_pc == other.train_pc
            && target_addr == other.target_addr
            && timestamp == other.timestamp
            && hit == other.hit;
    }

    bool operator!=(const sampler_entry& other) const {
        return !(*this == other);
    }

    sampler_entry& operator=(const sampler_entry& other) {
        if (this != &other) {  // Check for self-assignment
            this->addr_tag = other.addr_tag;
            this->train_pc = other.train_pc;
            this->target_addr = other.target_addr;
            this->timestamp = other.timestamp;
            this->hit = other.hit;
        }
        return *this;
    }
};

struct ssampler_entry{
    uint64_t target_addr;
    uint64_t train_pc;
    uint64_t timestamp;
    bool hit;

    ssampler_entry(){
        train_pc = 0;
        target_addr = 0;
        timestamp = 0;
        hit = 0;
    }

    ssampler_entry(uint64_t target_addr, uint64_t ip, uint64_t timestamp):target_addr(target_addr), train_pc(ip), timestamp(timestamp), hit(0){}

    ssampler_entry& operator=(const ssampler_entry& other) {
        if (this != &other) {  // Check for self-assignment
            this->train_pc = other.train_pc;
            this->target_addr = other.target_addr;
            this->timestamp = other.timestamp;
            this->hit = other.hit;
        }
        return *this;
    }
    bool operator==(const ssampler_entry& other) const {
        return train_pc == other.train_pc
            && target_addr == other.target_addr
            && timestamp == other.timestamp
            && hit == other.hit;
    }

    bool operator!=(const ssampler_entry& other) const {
        return !(*this == other);
    }
};

using reuse_entry = uint64_t;

template <class T>
class FIFOCache // using FIFOcache to replace sampler/ssampler/reuse/duller entry
{
private:
  struct DLinkedNode {
    uint64_t key;
    T value;
    DLinkedNode* pre;
    DLinkedNode* post;
  };
  unordered_map<uint64_t, DLinkedNode*> cache;

  uint64_t capacity;
  DLinkedNode* head;
  DLinkedNode* tail;

public:
  uint64_t count;

  FIFOCache(uint64_t capacity)
  {
    this->count = 0;
    this->capacity = capacity;

    head = new DLinkedNode();
    head->pre = nullptr;

    tail = new DLinkedNode();
    tail->post = nullptr;

    head->post = tail;
    tail->pre = head;
  }


  FIFOCache(const FIFOCache& other) : count(other.count), capacity(other.capacity) {

    head = new DLinkedNode();
    head->pre = nullptr;
    tail = new DLinkedNode();
    tail->post = nullptr;
    head->post = tail;
    tail->pre = head;


    DLinkedNode* curr = other.head->post;
    while (curr != other.tail) {
        DLinkedNode* newNode = new DLinkedNode();
        newNode->key = curr->key;
        newNode->value = curr->value;
        cache[curr->key] = newNode;
        addNode(newNode);
        curr = curr->post;
    }
}


FIFOCache(FIFOCache&& other) noexcept
    : count(other.count)
    , capacity(other.capacity)
    , head(other.head)
    , tail(other.tail)
    , cache(std::move(other.cache))
{

    other.head = nullptr;
    other.tail = nullptr;
    other.count = 0;
}


FIFOCache& operator=(FIFOCache&& other) noexcept {
    if (this != &other) {

        clear();


        count = other.count;
        capacity = other.capacity;
        head = other.head;
        tail = other.tail;
        cache = std::move(other.cache);


        other.head = nullptr;
        other.tail = nullptr;
        other.count = 0;
    }
    return *this;
}

  ~FIFOCache(){
    clear();
  }

  void clear() {
    if (head != nullptr) {
        DLinkedNode* current = head->post;
        while (current != tail) {
            DLinkedNode* next = current->post;
            delete current;
            current = next;
        }
        delete head;
        delete tail;

        head = nullptr;
        tail = nullptr;
        count = 0;
        cache.clear();
    }
}

  class Iterator {
  private:
    DLinkedNode* node;
  public:
    Iterator(DLinkedNode* pNode) : node(pNode) {}

    T& operator*() const { return node->value; }
    T* operator->() { return &(node->value); }

    // Prefix increment
    Iterator& operator++() {
      node = node->post;
      return *this;
    }

    // Postfix increment
    Iterator operator++(int) {
      Iterator iterator = *this;
      ++(*this);
      return iterator;
    }

    bool operator==(const Iterator& other) const { return node == other.node; }
    bool operator!=(const Iterator& other) const { return node != other.node; }
  };


  T* get(uint64_t key)
  {
    if (cache.find(key) == cache.end()) {
      return nullptr;
    }
    DLinkedNode* node = cache[key];
    return &(node->value);
  }

  void erase(uint64_t key)
  {
      auto it = cache.find(key);
      if (it == cache.end()) {
          return; // key not found
      }

      DLinkedNode* node = it->second;

      removeNode(node);      // unlink from FIFO linked list
      cache.erase(it);       // remove from hash table
      delete node;           // free memory
      --count;

      return;
  }

  T set(uint64_t key, T value)
  {
    if (cache.find(key) == cache.end()) {
      DLinkedNode* newNode = new DLinkedNode();
      newNode->key = key;
      newNode->value = value;

      cache[key] = newNode;
      addNode(newNode);

      ++count;

      if (count > capacity) {
        DLinkedNode* tailNode = popTail();
        cache.erase(tailNode->key);
        --count;
        T record = tailNode->value;
        delete tailNode;
        return record;
      }
    } else {
      DLinkedNode* node = cache[key];
      node->value = value;
    }
    return value;
  }

  Iterator begin() { return Iterator(head->post); }
  Iterator end() { return Iterator(tail); }

private:
  void addNode(DLinkedNode* node)
  {
    node->pre = head;
    node->post = head->post;

    head->post->pre = node;
    head->post = node;
  }

  void removeNode(DLinkedNode* node)
  {
    DLinkedNode* pre = node->pre;
    DLinkedNode* post = node->post;

    pre->post = post;
    post->pre = pre;
  }

  void moveToHead(DLinkedNode* node)
  {
    removeNode(node);
    addNode(node);
  }

  DLinkedNode* popTail()
  {
    DLinkedNode* res = tail->pre;
    removeNode(res);
    return res;
  }
};

template <class T>
class LRUCache // using LRUcache to record last recent access
{
private:
  struct DLinkedNode {
    uint64_t key;
    T value;
    DLinkedNode* pre;
    DLinkedNode* post;
  };
  unordered_map<uint64_t, DLinkedNode*> cache;
  int count;
  int capacity;
  DLinkedNode* head;
  DLinkedNode* tail;

public:
  LRUCache(int capacity)
  {
    this->count = 0;
    this->capacity = capacity;

    head = new DLinkedNode();
    head->pre = nullptr;

    tail = new DLinkedNode();
    tail->post = nullptr;

    head->post = tail;
    tail->pre = head;
  }


  LRUCache(const LRUCache& other) : count(other.count), capacity(other.capacity) {

    head = new DLinkedNode();
    head->pre = nullptr;
    tail = new DLinkedNode();
    tail->post = nullptr;
    head->post = tail;
    tail->pre = head;


    DLinkedNode* curr = other.head->post;
    while (curr != other.tail) {
        DLinkedNode* newNode = new DLinkedNode();
        newNode->key = curr->key;
        newNode->value = curr->value;
        cache[curr->key] = newNode;
        addNode(newNode);
        curr = curr->post;
    }
  }


  LRUCache(LRUCache&& other) noexcept
  : count(other.count)
  , capacity(other.capacity)
  , head(other.head)
  , tail(other.tail)
  , cache(std::move(other.cache))
  {

  other.head = nullptr;
  other.tail = nullptr;
  other.count = 0;
  }


  LRUCache& operator=(LRUCache&& other) noexcept {
  if (this != &other) {

      clear();


      count = other.count;
      capacity = other.capacity;
      head = other.head;
      tail = other.tail;
      cache = std::move(other.cache);

      other.head = nullptr;
      other.tail = nullptr;
      other.count = 0;
  }
  return *this;
  }

  ~LRUCache(){
      clear();
  }

  void clear() {
      if (head != nullptr) {
          DLinkedNode* current = head->post;
          while (current != tail) {
              DLinkedNode* next = current->post;
              delete current;
              current = next;
          }
          delete head;
          delete tail;

          head = nullptr;
          tail = nullptr;
          count = 0;
          cache.clear();
      }
  }

  int get_position(uint64_t key)
  {
      DLinkedNode* cur = head->post;
      int position = 0;

      while (cur != tail) {
          if (cur->key == key)
              return position;

          cur = cur->post;
          position++;
      }

      return -1;
  }

  T* get(uint64_t key)
  {
    if (cache.find(key) == cache.end()) {
      return nullptr;
    }
    DLinkedNode* node = cache[key];
    moveToHead(node);

    return &(node->value);
  }

  T set(uint64_t key, T value)
  {
    if (cache.find(key) == cache.end()) {
      DLinkedNode* newNode = new DLinkedNode();
      newNode->key = key;
      newNode->value = value;

      cache[key] = newNode;
      addNode(newNode);

      ++count;

      if (count > capacity) {
        DLinkedNode* tailNode = popTail();
        cache.erase(tailNode->key);
        --count;
        T record = tailNode->value;
        delete tailNode;
        return record;
      }
    }
    else {
      DLinkedNode* node = cache[key];
      node->value = value;
      moveToHead(node);
    }
    return value;
  }

  DLinkedNode* begin() { return head; }

  DLinkedNode* end() { return tail; }

private:
  void addNode(DLinkedNode* node)
  {
    node->pre = head;
    node->post = head->post;

    head->post->pre = node;
    head->post = node;
  }

  void removeNode(DLinkedNode* node)
  {
    DLinkedNode* pre = node->pre;
    DLinkedNode* post = node->post;

    pre->post = post;
    post->pre = pre;
  }

  void moveToHead(DLinkedNode* node)
  {
    removeNode(node);
    addNode(node);
  }

  DLinkedNode* popTail()
  {
    DLinkedNode* res = tail->pre;
    removeNode(res);
    return res;
  }
};

template <class T>
class LRURPSetAssocCache
{
private:
    struct Entry {
        bool valid;
        uint64_t key;
        T value;
        uint32_t lru;   // 0 = MRU, larger = less recently used

        Entry() : valid(false), key(0), value(), lru(0) {}
    };

    int capacity;
    int assoc;
    int num_sets;
    int way_allocation_max;
    std::vector<std::vector<Entry>> sets;

    uint64_t get_set(uint64_t key) const
    {
        return key % static_cast<uint64_t>(num_sets);
    }

    uint64_t normalize_set_index(uint64_t set_index) const
    {
        return set_index % static_cast<uint64_t>(num_sets);
    }

    int active_ways() const
    {
        return std::min(assoc, std::max(1, way_allocation_max));
    }

    void normalize_lru(std::vector<Entry>& set)
    {
        std::vector<int> valid_ways;
        for (int i = 0; i < active_ways(); ++i) {
            if (set[i].valid)
                valid_ways.push_back(i);
        }

        std::sort(valid_ways.begin(), valid_ways.end(), [&](int a, int b) {
            return set[a].lru < set[b].lru;
        });

        for (uint32_t pos = 0; pos < valid_ways.size(); ++pos)
            set[valid_ways[pos]].lru = pos;
    }

    void touch(std::vector<Entry>& set, int way)
    {
        const uint32_t old_lru = set[way].lru;

        for (int i = 0; i < active_ways(); ++i) {
            if (!set[i].valid || i == way)
                continue;

            if (set[i].lru < old_lru)
                set[i].lru++;
        }

        set[way].lru = 0;
    }

    void install_as_mru(std::vector<Entry>& set, int way, uint64_t key, const T& value)
    {
        for (int i = 0; i < active_ways(); ++i) {
            if (set[i].valid && i != way)
                set[i].lru++;
        }

        set[way].valid = true;
        set[way].key = key;
        set[way].value = value;
        set[way].lru = 0;
    }

    int find_way(const std::vector<Entry>& set, uint64_t key) const
    {
        const int ways = std::min(assoc, std::max(1, way_allocation_max));
        for (int way = 0; way < ways; ++way) {
            if (set[way].valid && set[way].key == key)
                return way;
        }
        return -1;
    }

    int find_invalid_way(const std::vector<Entry>& set) const
    {
        for (int way = 0; way < active_ways(); ++way) {
            if (!set[way].valid)
                return way;
        }
        return -1;
    }

    int find_lru_victim_way(const std::vector<Entry>& set) const
    {
        int victim = 0;
        for (int way = 1; way < active_ways(); ++way) {
            if (set[way].lru > set[victim].lru)
                victim = way;
        }
        return victim;
    }

public:
    int count;

    LRURPSetAssocCache(int capacity, int assoc)
        : capacity(capacity), assoc(assoc), count(0)
    {
        if (this->assoc <= 0)
            this->assoc = 1;
        if (this->capacity <= 0)
            this->capacity = this->assoc;

        num_sets = std::max(1, this->capacity / this->assoc);
        way_allocation_max = this->assoc;
        sets.assign(num_sets, std::vector<Entry>(this->assoc));
    }

    int get_num_sets() const { return num_sets; }
    int get_assoc() const { return assoc; }
    int get_way_allocation_max() const { return way_allocation_max; }

    void set_way_allocation_max(int new_way_allocation_max)
    {
        way_allocation_max = std::min(assoc, std::max(1, new_way_allocation_max));

        // Entries in disabled physical ways are no longer visible, matching gem5's
        // way-allocation cap for the rounded metadata table.
        for (auto& set : sets) {
            for (int way = way_allocation_max; way < assoc; ++way) {
                if (set[way].valid) {
                    set[way] = Entry();
                    --count;
                }
            }
            normalize_lru(set);
        }
    }

    void clear()
    {
        for (auto& set : sets) {
            for (auto& entry : set)
                entry = Entry();
        }
        count = 0;
    }

    T* find_entry(uint64_t key)
    {
        return find_entry(key, get_set(key));
    }

    const T* find_entry(uint64_t key) const
    {
        return find_entry(key, get_set(key));
    }

    T* find_entry(uint64_t key, uint64_t set_index)
    {
        auto& set = sets[normalize_set_index(set_index)];
        int way = find_way(set, key);

        if (way < 0)
            return nullptr;

        return &(set[way].value);
    }

    const T* find_entry(uint64_t key, uint64_t set_index) const
    {
        const auto& set = sets[set_index % static_cast<uint64_t>(num_sets)];
        int way = find_way(set, key);

        if (way < 0)
            return nullptr;

        return &(set[way].value);
    }

    T* find(uint64_t key)
    {
        return find_entry(key);
    }

    const T* find(uint64_t key) const
    {
        return find_entry(key);
    }

    T* access_entry(uint64_t key)
    {
        return access_entry(key, get_set(key));
    }

    T* access_entry(uint64_t key, uint64_t set_index)
    {
        auto& set = sets[normalize_set_index(set_index)];
        int way = find_way(set, key);

        if (way < 0)
            return nullptr;

        touch(set, way);
        return &(set[way].value);
    }

    T* get(uint64_t key)
    {
        return access_entry(key);
    }

    T* get(uint64_t key, uint64_t set_index)
    {
        return access_entry(key, set_index);
    }

    void erase(uint64_t key)
    {
        erase(key, get_set(key));
    }

    void erase(uint64_t key, uint64_t set_index)
    {
        auto& set = sets[normalize_set_index(set_index)];
        int way = find_way(set, key);

        if (way < 0)
            return;

        set[way] = Entry();
        --count;
        normalize_lru(set);
    }

    T set(uint64_t key, T value)
    {
        return set(key, value, get_set(key));
    }

    T set(uint64_t key, T value, uint64_t set_index)
    {
        auto& set = sets[normalize_set_index(set_index)];

        int way = find_way(set, key);
        if (way >= 0) {
            T old_value = set[way].value;
            install_as_mru(set, way, key, value);
            return old_value;
        }

        way = find_invalid_way(set);
        if (way >= 0) {
            install_as_mru(set, way, key, value);
            ++count;
            return value;
        }

        way = find_lru_victim_way(set);
        T evicted_value = set[way].value;
        install_as_mru(set, way, key, value);
        return evicted_value;
    }

    std::vector<std::pair<uint64_t, T>> snapshot_entries() const
    {
        std::vector<std::pair<uint64_t, T>> entries;
        entries.reserve(count > 0 ? static_cast<size_t>(count) : 0);

        for (const auto& set : sets) {
            for (const auto& entry : set) {
                if (entry.valid)
                    entries.emplace_back(entry.key, entry.value);
            }
        }

        return entries;
    }

    template <typename Predicate>
    void invalidate_if(Predicate should_invalidate)
    {
        for (auto& set : sets) {
            for (int way = 0; way < assoc; ++way) {
                if (set[way].valid && should_invalidate(set[way].key, set[way].value)) {
                    set[way] = Entry();
                    --count;
                }
            }
            normalize_lru(set);
        }
    }
};


struct duller_entry{
    uint64_t set = 0;
    uint64_t setMask = 0;
    uint64_t temporalMod = 0;
    uint64_t temporalModMax = 0;
    uint64_t cacheMaxAssoc = 0;

    std::vector<uint64_t> cacheAddrs;
    std::vector<uint64_t> cacheAddrTick;
    std::vector<uint64_t> temporalAddrs;
    std::vector<uint64_t> temporalAddrTick;
    std::vector<bool> inserted;

    duller_entry() = default;

    void reset(uint64_t mask, uint64_t modMax, uint64_t cacheAssoc, std::mt19937_64& rng)
    {
        setMask = mask;
        temporalModMax = modMax;
        cacheMaxAssoc = cacheAssoc;

        cacheAddrs.assign(cacheMaxAssoc, 0);
        cacheAddrTick.assign(cacheMaxAssoc, 0);
        temporalAddrs.assign(cacheMaxAssoc, 0);
        temporalAddrTick.assign(cacheMaxAssoc, 0);
        inserted.assign(cacheMaxAssoc, false);

        std::uniform_int_distribution<uint64_t> set_dist(0, setMask);
        std::uniform_int_distribution<uint64_t> mod_dist(0, temporalModMax - 1);

        set = set_dist(rng);
        temporalMod = mod_dist(rng);
    }

    int checkAndInsert(uint64_t addr, bool should_pf, uint64_t tick)
    {
        int ret = 0;
        bool foundInCache = false;
        bool foundInTemp = false;

        if ((addr & setMask) != set)
            return ret;

        for (uint64_t x = 0; x < cacheMaxAssoc; ++x) {
            if (addr == cacheAddrs[x]) {
                foundInCache = true;

                int index = static_cast<int>(cacheMaxAssoc) - 1;
                for (uint64_t y = 0; y < cacheMaxAssoc; ++y) {
                    if (cacheAddrTick[x] > cacheAddrTick[y])
                        index--;
                }

                cacheAddrTick[x] = tick;
                ret += index + 1;
            }

            if (should_pf && addr == temporalAddrs[x]) {
                foundInTemp = true;

                int index = static_cast<int>(cacheMaxAssoc) - 1;
                for (uint64_t y = 0; y < cacheMaxAssoc; ++y) {
                    if (temporalAddrTick[x] > temporalAddrTick[y])
                        index--;
                }

                ret += 128 * (index + 1);
                temporalAddrTick[x] = tick;
                inserted[x] = true;
            }
        }

        if (!foundInCache) {
            uint64_t oldestTick = std::numeric_limits<uint64_t>::max();
            uint64_t victim = 0;
            for (uint64_t x = 0; x < cacheMaxAssoc; ++x) {
                if (cacheAddrTick[x] < oldestTick) {
                    victim = x;
                    oldestTick = cacheAddrTick[x];
                }
            }
            cacheAddrs[victim] = addr;
            cacheAddrTick[victim] = tick;
        }

        if (!foundInTemp && should_pf && (((addr / (setMask + 1)) % temporalModMax) == temporalMod)) {
            uint64_t oldestTick = std::numeric_limits<uint64_t>::max();
            uint64_t victim = 0;
            for (uint64_t x = 0; x < cacheMaxAssoc; ++x) {
                if (temporalAddrTick[x] < oldestTick) {
                    victim = x;
                    oldestTick = temporalAddrTick[x];
                }
            }
            temporalAddrs[victim] = addr;
            temporalAddrTick[victim] = tick;
            inserted[victim] = false;
        }

        return ret;
    }
};

template<typename T>
void removeDuplicates(std::vector<T>& vec) {
    std::sort(vec.begin(), vec.end());
    auto last = std::unique(vec.begin(), vec.end());
    vec.erase(last, vec.end());
}

class triangel: public champsim::modules::prefetcher{
    constexpr static bool TRIANGEL_DEBUG_PRINT = true;

    //set the parameters of table size
    static constexpr int16_t train_size = 512;
    static constexpr int16_t train_assoc = 16;
    static constexpr int16_t sampler_size = 512;
    static constexpr int16_t sampler_assoc = 2;
    static constexpr int16_t ssampler_size = 64;
    static constexpr int16_t reuse_size = 512;
    static constexpr int16_t duller_size = 64;
    static constexpr uint64_t window_size = 500000;

    static constexpr uint8_t address_map_actual_cache_assoc = 12;
    static constexpr uint8_t address_map_rounded_cache_assoc = 16;

    // Initialized once from the shared LLC geometry in set_llc_reference().
    static uint32_t max_metadata_ways;
    static uint64_t address_map_actual_entries;
    static uint64_t address_map_rounded_entries;
    static uint64_t address_map_num_sets;
    static uint64_t size_increment;
    static uint64_t max_size;
    uint64_t timestamp = 0;

    //set the structure of Triangle
    LRURPSetAssocCache<train_table_entry> train_table{train_size, train_assoc};
    LRURPSetAssocCache<sampler_entry> sampler{sampler_size, sampler_assoc};
    FIFOCache<ssampler_entry> ssampler{ssampler_size};
    FIFOCache<reuse_entry> reuse_buffer{reuse_size};
    static LRURPSetAssocCache<metadata> markov_table;
    uint64_t markov_table_accesses = 0;

    static constexpr uint8_t NUM_SIZE_DUELERS = 64;

    static std::vector<duller_entry> size_duels;
    static bool size_duels_initialized;
    static std::mt19937_64 size_duel_rng;
    static std::mt19937_64 rng;

    static std::vector<uint64_t> set_prefetch_score;
    static uint64_t all;
    static uint64_t repartition;
    static uint64_t size_duel_tick;

    // Number of LLC ways currently reserved for metadata.
    // This corresponds to gem5 current_size / size_increment.
    static uint8_t current_metadata_ways;

    static CACHE* llc_cache;

    CACHE* target_cache = nullptr;

    //calculata the infomation
    uint64_t sample_entry_number = 0;

    uint32_t metadata_hash(uint64_t value) const {
        uint32_t lower = value & 0xFFFFFFFF;
        uint32_t upper = (value >> 32) & 0xFFFFFFFF;
        uint32_t mixed = lower ^ upper;
        mixed = mixed ^ (mixed >> 16);
        mixed = mixed ^ (mixed >> 8);
        return mixed;
    }

    uint64_t metadata_set_index(uint64_t addr, uint32_t active_metadata_ways) const {
        if (active_metadata_ways == 0 || address_map_num_sets == 0)
            return 0;

        const uint64_t active = std::min<uint32_t>(active_metadata_ways, max_metadata_ways);
        const uint64_t offset = (addr * max_metadata_ways) + (metadata_hash(addr) % active);
        return offset % address_map_num_sets;
    }

    bool metadata_entry_active(uint64_t addr, uint32_t active_metadata_ways) const {
        if (active_metadata_ways == 0)
            return false;

        return (metadata_set_index(addr, active_metadata_ways) % max_metadata_ways) < active_metadata_ways;
    }

    void apply_metadata_partition(uint32_t active_metadata_ways) {
        auto entries = markov_table.snapshot_entries();
        markov_table.clear();

        if (active_metadata_ways == 0)
            return;

        for (const auto& entry : entries) {
            const uint64_t metadata_key = entry.first;
            if (!metadata_entry_active(metadata_key, active_metadata_ways))
                continue;

            markov_table.set(metadata_key, entry.second,
                             metadata_set_index(metadata_key, active_metadata_ways));
        }
    }

    bool should_sample(uint32_t sample_rate) {

      static std::mt19937_64 rng{0x9E3779B97F4A7C15ULL};

      //Using fixed-point arithmetic instead of floating-point to avoid precision issues
      const uint64_t PRECISION = 1000000000ULL;

      uint64_t probability = PRECISION * sampler_size / max_size;

      // Handle cases where sample_rate is >= 8 or < 8 differently
      if (sample_rate >= 8) {

           // Left shift for sample_rate >= 8 (multiply by 2^(sample_rate-8))
          probability *= (1ULL << (sample_rate - 8));
      } else {

          // Right shift for sample_rate < 8 (divide by 2^(8-sample_rate))
          probability /= (1ULL << (8 - sample_rate));
      }

      uint64_t random = std::uniform_int_distribution<uint64_t>{0, PRECISION}(rng);

      // Return true if random number is less than calculated probability
      return probability >= random;
    }


    uint64_t tem_pre(uint64_t last_addr)
    {
        if (current_metadata_ways == 0)
            return 0;

        const uint64_t metadata_key = last_addr;
        const uint64_t metadata_set = metadata_set_index(metadata_key, current_metadata_ways);

        markov_table_accesses++;
        metadata* cur_addr = markov_table.get(metadata_key, metadata_set);

        if (!cur_addr) {
            return 0;
        }

        return cur_addr->target_addr;
    }

    void initialize_size_duels()
    {
        if (size_duels_initialized)
            return;
        assert(llc_cache != nullptr);
        const uint64_t duel_sets = size_increment / address_map_actual_cache_assoc;
        assert(duel_sets > 0);
        assert((duel_sets & (duel_sets - 1)) == 0);

        const uint64_t set_mask = duel_sets - 1;

        for (auto& duel : size_duels) {
            duel.reset(set_mask, address_map_actual_cache_assoc, llc_cache->NUM_WAY, size_duel_rng);
        }

        size_duels_initialized = true;
    }

    void process_size_dueling_access(uint64_t addr, bool should_pf)
    {
        initialize_size_duels();
        
        const uint64_t tick = ++size_duel_tick;
        all++;

        for (auto& duel : size_duels) {
            int res = duel.checkAndInsert(addr, should_pf, tick);
            if (res == 0)
                continue;

            const int cache_hit = res % 128;
            const int pref_hit = res / 128;
            const int cache_set = cache_hit - 1;
            const int pref_set = pref_hit - 1;

            if (cache_hit) {
                const int score_count = static_cast<int>(set_prefetch_score.size());
                for (int y = score_count - 2 - cache_set; y >= 0; --y)
                    set_prefetch_score[static_cast<std::size_t>(y)]++;
            }

            if (pref_hit) {
                const uint64_t pref_weight = (2 * duel.temporalModMax) / 4;

                const int score_count = static_cast<int>(set_prefetch_score.size());
                for (int y = pref_set + 1; y < score_count; ++y)
                    set_prefetch_score[static_cast<std::size_t>(y)] += pref_weight;
            }
        }

        if (all > window_size) {
            update_partition();
        }
    }

    void update_cache_side_duel(uint64_t addr)
    {
        initialize_size_duels();

        const uint64_t tick = ++size_duel_tick;
        all++;

        for (auto& duel : size_duels) {
            int res = duel.checkAndInsert(addr, false, tick);
            if (res == 0)
                continue;

            const int cache_hit = res % 128;
            const int cache_set = cache_hit - 1;

            if (cache_hit) {
                const int score_count = static_cast<int>(set_prefetch_score.size());
                for (int y = score_count - 2 - cache_set; y >= 0; --y)
                    set_prefetch_score[static_cast<std::size_t>(y)]++;
            }
        }

        if (all > window_size) {
            update_partition();
        }
    }

    void update_partition()
    {
        uint32_t target_metadata_ways = current_metadata_ways;
        uint64_t best_score = 0;

        for (uint32_t x = 0; x <= max_metadata_ways; ++x) {
            if (set_prefetch_score[x] > best_score) {
                best_score = set_prefetch_score[x];
                target_metadata_ways = x;
            }
        }

        // Small bias against changing:
        //   currentscore = currentscore + (currentscore >> 4)
        //
        // This means target must beat current by about 6.25%.
        uint64_t current_score = set_prefetch_score[current_metadata_ways];
        uint64_t biased_current_score = current_score + (current_score >> 4);
        uint64_t target_score = set_prefetch_score[target_metadata_ways];

        bool should_change = (target_metadata_ways != current_metadata_ways) && (target_score > biased_current_score);

        if (should_change) {
            const uint8_t new_metadata_ways =  std::min<uint32_t>(target_metadata_ways, max_metadata_ways);
            
            uint32_t llc_available_ways = llc_cache->NUM_WAY;
            llc_available_ways = llc_cache->NUM_WAY > new_metadata_ways ? llc_cache->NUM_WAY - new_metadata_ways : 1;
            llc_cache->set_available_ways(llc_available_ways);

            current_metadata_ways = new_metadata_ways;
            repartition++;

            apply_metadata_partition(current_metadata_ways);

            if constexpr (TRIANGEL_DEBUG_PRINT) {
                std::cout << "[TRIANGEL] LLC partition updated: metadata_ways="
                          << static_cast<uint32_t>(current_metadata_ways)
                          << ", rounded_entries="
                          << address_map_rounded_entries
                          << ", rounded_assoc="
                          << static_cast<uint32_t>(address_map_rounded_cache_assoc)
                          << ", usable_assoc="
                          << static_cast<uint32_t>(address_map_actual_cache_assoc)
                          << ", available_ways="
                          << llc_available_ways
                          << std::endl;
            }
        }

        if constexpr (TRIANGEL_DEBUG_PRINT) {
            std::cout << "[TRIANGEL] End of set-dueling epoch\n";
            for (uint32_t x = 0; x <= max_metadata_ways; ++x) {
                std::cout << "  metadata_ways=" << static_cast<uint32_t>(x)
                          << " score=" << set_prefetch_score[x] << std::endl;
            }
        }

        std::fill(set_prefetch_score.begin(), set_prefetch_score.end(), 0);

        all = 0;
    }

public:
    // Prefetch candidate produced by prefetch_candidates(). Mirrors the layout expected by the
    // Pumpkin sub-prefetcher framework (pumpkin_wrappers::WrappedPrefetch) so a wrapper can forward
    // triangel's predictions to a meta-prefetcher (pumpkin / alecto / naive / ipop / ...).
    struct Candidate {
        champsim::address address{};
        champsim::address trigger_ip{};
        bool fill_this_level = true;
        uint32_t metadata = 0;
        std::function<void(bool issued, bool filtered_out)> notify_result{};
    };

    // Runs the same training/prediction path as prefetcher_cache_operate, but returns the predicted
    // lines as candidates instead of issuing them directly via prefetch_line(). When allow_training
    // is false, the training update (log()) is skipped for this access while prediction still runs.
    std::vector<Candidate> prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                               uint32_t metadata_in, bool allow_training = true);

    void print_info(){
      cout<<"Train table entries: "<<train_table.count<<endl;
      cout<<"Sampler entries: "<<sampler.count<<endl;
      cout<<"Ssampler entries: "<<ssampler.count<<endl;
      cout<<sample_entry_number<<endl;
    }

    void set_llc_reference(CACHE* llc) {
        if (llc == nullptr)
            assert(false);

        if (llc_cache != nullptr) {
            assert(llc_cache == llc);
            return;
        }

        llc_cache = llc;

        max_metadata_ways = static_cast<uint32_t>(llc_cache->NUM_WAY / 2);
        const uint64_t llc_lines = static_cast<uint64_t>(llc_cache->NUM_SET) * llc_cache->NUM_WAY;
        assert((llc_lines % 2) == 0);

        address_map_num_sets = llc_lines / 2;
        address_map_actual_entries = address_map_num_sets * address_map_actual_cache_assoc;
        address_map_rounded_entries = address_map_num_sets * address_map_rounded_cache_assoc;
        size_increment = address_map_actual_entries / max_metadata_ways;
        max_size = address_map_actual_entries;

        assert(address_map_actual_entries % max_metadata_ways == 0);
        assert(address_map_actual_entries / address_map_actual_cache_assoc ==
               address_map_rounded_entries / address_map_rounded_cache_assoc);

        markov_table = LRURPSetAssocCache<metadata>(
            static_cast<int>(address_map_rounded_entries),
            address_map_rounded_cache_assoc);
        markov_table.set_way_allocation_max(address_map_actual_cache_assoc);

        set_prefetch_score.assign(static_cast<std::size_t>(llc_cache->NUM_WAY) + 1, 0);

        current_metadata_ways = 0;
        all = 0;
        size_duel_tick = 0;
        size_duels_initialized = false;

        llc_cache->set_available_ways(llc_cache->NUM_WAY);
    }

    //define the functions which used by Triangle
    void log( uint64_t ip, uint64_t addr){

      //store the ip not appear before or make the metadata
      auto cur = train_table.find_entry(ip);


      if(cur != nullptr){
        train_table.access_entry(ip);
        if(addr == cur->last_addr1) return;
        if(cur->pattern_conf1 >= 14) cur->lookahead =true;
        if(cur->pattern_conf0 < 7) cur->lookahead =false;

        uint32_t sampler_key = cur->last_addr1;
        sampler_entry access_A(sampler_key, ip, addr, cur->local_timestamp+1);

        const bool should_pf = (cur->reuse_conf > 7) && (cur->pattern_conf0 > 7);

        //Check second-chance sampler for a recent history. If so, update pattern confidence accordingly.
        auto scs = ssampler.get(addr);
        if (scs != nullptr && !scs->hit){
          scs->hit = true;
          auto scs_train = train_table.find_entry(scs->train_pc);
          if (scs_train != nullptr && (scs->timestamp + 512 > this->timestamp)){
            if (scs->train_pc == ip){
              if(scs_train->pattern_conf0<15) scs_train->pattern_conf0++;
              if(scs_train->pattern_conf1<15) scs_train->pattern_conf1++;
            }
          } else if (scs_train != nullptr) {
              if (scs_train->pattern_conf0 > 1) {
                scs_train->pattern_conf0 -= 2;
              } else {
                scs_train->pattern_conf0 = 0;
              }
              if (scs_train->pattern_conf1 > 4) {
                scs_train->pattern_conf1 -= 5;
              } else {
                scs_train->pattern_conf1 = 0;
              }
          }
        }

        //Update the status of train_entry
        //search history sampler to judge update patternconf and reuseconf
        sampler_entry* access_B = sampler.find_entry(sampler_key);

        if (access_B != nullptr && access_A.train_pc == access_B->train_pc) {
            int64_t distance = cur->local_timestamp - access_B->timestamp;

            if (distance > 0 && distance < max_size) {
              if (cur->reuse_conf < 15) cur->reuse_conf++;
            }
            else if (!access_B->hit) {
                if (cur->reuse_conf > 0) cur->reuse_conf--;
            }

            // Set the hit field of the actual sampler entry to true
            access_B->hit = true;

            const bool pattern_match = (access_A.target_addr == access_B->target_addr);

            const bool target_in_cache = target_cache != nullptr && target_cache->contains_nonprefetched_line(champsim::address{access_B->target_addr << LOG2_BLOCK_SIZE});

            if (pattern_match || target_in_cache) {
              if (pattern_match) {
                if (cur->pattern_conf0 < 15) cur->pattern_conf0++;
                if (cur->pattern_conf1 < 15) cur->pattern_conf1++;
              }
            }
            else {
              ssampler_entry access_a(access_B->target_addr, ip, timestamp);
              ssampler_entry access_b = ssampler.set(access_B->target_addr, access_a);

              if ((access_b != access_a) && !access_b.hit){
                auto evict_scs_train = train_table.find_entry(access_b.train_pc);
                if (evict_scs_train != nullptr) {
                  if (evict_scs_train->pattern_conf0 > 1) {
                    evict_scs_train->pattern_conf0 -= 2;
                  } else {
                    evict_scs_train->pattern_conf0 = 0;
                  }
                  if (evict_scs_train->pattern_conf1 > 4) {
                    evict_scs_train->pattern_conf1 -= 5;
                  } else {
                    evict_scs_train->pattern_conf1 = 0;
                  }
                }
              }
            }

            // Update the sampler's target addr with current addr
            access_B->target_addr = access_A.target_addr;
        } else if (should_sample(cur->sample_rate)){
          sampler_entry access_B = sampler.set(sampler_key, access_A);
          if (access_B!=access_A){
            sample_entry_number++;
            auto vic_train = train_table.find_entry(access_B.train_pc);
            if (vic_train != nullptr){
              int64_t distance = vic_train->local_timestamp - access_B.timestamp;
              if (distance > max_size){
                train_table.access_entry(access_B.train_pc);
                if(!access_B.hit && vic_train->reuse_conf>0) vic_train->reuse_conf--;
                if(cur->sample_rate<15) cur->sample_rate++;
              } else if(distance > 0 && !access_B.hit && cur->sample_rate>0){
                cur->sample_rate--;
              }
            } else {
              if(cur->sample_rate<15) cur->sample_rate++;
            }
          }
        }

        //according to partition ways to allocate metadata
        if ((current_metadata_ways > 0) && (cur->reuse_conf > 7) && (cur->pattern_conf0 > 7)) {

          const uint64_t metadata_key = cur->lookahead ? cur->last_addr0 : cur->last_addr1;
          const uint64_t metadata_set = metadata_set_index(metadata_key, current_metadata_ways);
          metadata cor_addr(metadata_key, addr);

          markov_table_accesses++;
          metadata* pre_cor_addr = markov_table.get(metadata_key, metadata_set);

          if (!pre_cor_addr) {
              markov_table.set(metadata_key, cor_addr, metadata_set);
          }
          else {
              bool was_confident = pre_cor_addr->conf != 0;
              bool now_confident = (pre_cor_addr->target_addr == cor_addr.target_addr);

              pre_cor_addr->conf = now_confident ? 1 : 0;

              if (!was_confident) {
                  pre_cor_addr->target_addr = cor_addr.target_addr;
                  pre_cor_addr->tag = cor_addr.tag;
              }
          }
        }

        process_size_dueling_access(addr, should_pf);

        if (timestamp==0xffffffff) timestamp=0;
        cur->last_addr0 = cur->last_addr1;
        cur->last_addr1 = addr;
        this->timestamp++;
        cur->timestamp = this->timestamp;
        cur->local_timestamp++;
      }

      else {
        if (should_sample(8)) {
          train_table_entry current(ip, addr, timestamp);
          train_table.set(ip, current);
          timestamp++;
        }

        process_size_dueling_access(addr, false);
      }
    }

    vector<uint64_t> predict(uint64_t ip, uint64_t addr)
    {
        vector<uint64_t> prefetch_candidate;

        if (addr == 0)
            return prefetch_candidate;

        if (current_metadata_ways == 0)
            return prefetch_candidate;

        train_table_entry* cur_ip = train_table.find_entry(ip);
        if (cur_ip == nullptr) return prefetch_candidate;

        bool should_pf = (cur_ip->reuse_conf > 7) && (cur_ip->pattern_conf0 > 7);

        if (!should_pf) return prefetch_candidate;

        bool high_degree_pf = (cur_ip->pattern_conf1 > 14);

        int max_degree = high_degree_pf ? 4 : 1;

        // First Markov prediction
        uint64_t next_addr = tem_pre(addr);

        if (next_addr != 0) {
            reuse_buffer.set(next_addr, next_addr);
        } else {
            return prefetch_candidate;
        }

        int issued = 0;

        while (next_addr != 0 && issued < max_degree) {
            prefetch_candidate.push_back(next_addr);
            issued++;

            if (issued < max_degree) {
                next_addr = tem_pre(next_addr);

                // Preserve original behavior for chained predictions too.
                if (next_addr != 0) {
                    reuse_buffer.set(next_addr, next_addr);
                }
            } else {
                next_addr = 0;
            }
        }

        return prefetch_candidate;
    }

    explicit triangel(CACHE* cache)
        : champsim::modules::prefetcher(cache),
          target_cache(cache)
    {
    }

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                        uint32_t metadata_in);
    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
    void prefetcher_cycle_operate();
    void prefetcher_final_stats();

};
