#include "triangel.h"

//#define DEBUG

#ifdef DEBUG

set<uint64_t> critic_IP{};
uint64_t total_metadata = 0;
uint64_t total_metadata1 = 0;
uint64_t time_pre_fail = 0;
uint64_t time_pre_success = 0;

#endif


std::vector<triangel::Candidate> triangel::prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                               access_type type, uint32_t metadata_in, bool allow_training)
{
    (void)metadata_in;

    std::vector<Candidate> candidates;

    uint64_t line_addr = (addr.to<uint64_t>() >> LOG2_BLOCK_SIZE);
    uint64_t ip_value = ip.to<uint64_t>();

    if (cache_hit && !useful_prefetch) {
        return candidates;
    }

    bool is_store = (type == access_type::WRITE);

    if (is_store) {
        update_cache_side_duel(line_addr);
        return candidates;
    }

    if (allow_training) {
        this->log(ip_value, line_addr);
    }

    std::vector<uint64_t> prefetch_candidate = this->predict(ip_value, line_addr);

    removeDuplicates(prefetch_candidate);

    for (uint64_t pf_line_addr : prefetch_candidate) {
        if (pf_line_addr == 0)
            break;

        uint64_t p_addr = pf_line_addr << LOG2_BLOCK_SIZE;
        candidates.push_back({champsim::address{p_addr}, ip, true, 0, {}});
    }

    return candidates;
}

uint32_t triangel::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in)
{
    for (const auto& candidate : prefetch_candidates(addr, ip, cache_hit, useful_prefetch, type, metadata_in)) {
        prefetch_line(candidate.address, candidate.fill_this_level, candidate.metadata);
    }

    return metadata_in;
}

uint32_t triangel::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  //uint64_t line_addr = (addr.to<uint64_t>() >> LOG2_BLOCK_SIZE);            // Line addr
  //uint64_t line_evicted = (evicted_addr >> LOG2_BLOCK_SIZE); // Line addr

  // Add to the shadow cache
  //this->corres_cache_add(set, way, line_addr, prefetch);
  return metadata_in;
}

void triangel::prefetcher_final_stats() {
  cout<<"Repartition times: "<<repartition<<'\n';
  cout<<"Critic IP: "<<critic_IP.size()<<endl;
  cout<<"Markov Insert Entries: "<<total_metadata<<endl;
  cout<<"Markov Entries: "<<total_metadata1<<endl;
  cout<<"Metadata_Miss: "<<time_pre_fail<<endl;
  cout<<"Metadata_Hit: "<<time_pre_success<<endl;
  for (const auto &item : in_metadata){
    if (item.second == true) useful_entry_number++;
    else useless_entry_number++;
  }
  cout<<"Useful_Entry: "<<useful_entry_number<<endl;
  cout<<"Useless_Entry: "<<useless_entry_number<<endl;
 }

void triangel::prefetcher_cycle_operate() {
  current_cycle++;
}