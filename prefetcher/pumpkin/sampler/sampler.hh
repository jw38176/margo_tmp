#ifndef PUMPKIN_SAMPLER_H
#define PUMPKIN_SAMPLER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include<vector> 

#include "address.h"
#include "msl/lru_table.h"

class Sampler {
  public:
    explicit Sampler(std::size_t sampler_sets, std::size_t sampler_ways);
    ~Sampler();

    void init(std::size_t num_prefetchers);

    struct sampler_entry {
      
      champsim::address               pf_line_addr; 
      uint64_t                        fill_latency;      
      uint64_t                        first_used_timestamp;  
      std::vector<uint64_t>           prefetch_timestamps; // Length == _num_prefetchers
      std::vector<champsim::address>  prefetch_src_pcs; // Length == _num_prefetchers

      auto index() const
      {
        using namespace champsim::data::data_literals;
        return pf_line_addr.slice_upper<2_b>();
      }
      auto tag() const
      {
        using namespace champsim::data::data_literals;
        return pf_line_addr.slice_upper<2_b>();
      }
    };

    bool should_sample();

    // Returns evicted entry 
    std::optional<sampler_entry> record_prefetch(champsim::address pf_line_addr, uint64_t cycle, champsim::address src_pc, std::size_t prefetcher_id);

    void mark_filled(champsim::address pf_line_addr, uint64_t cycle);

    std::optional<sampler_entry> 
    check_and_evict_entry(champsim::address line_addr);

    void retune_train_ratio(uint64_t cache_evicts, uint64_t table_evicts);

  private:

    std::size_t _num_prefetchers{0};

    // Sampler Parameters
    std::size_t _sampler_sets;
    std::size_t _sampler_ways;
    static constexpr uint64_t MAX_TRAIN_RATIO = 1024;
    static constexpr uint64_t MIN_TRAIN_RATIO = 2;

    uint64_t _train_ratio{MIN_TRAIN_RATIO};
    uint64_t _rng_state{0};
    uint64_t rand_u64();
    
    
    // Sampler Table
    champsim::msl::lru_table<sampler_entry> _table{_sampler_sets, _sampler_ways};
};

#endif
