#include "triangel.h"

//The following 7 things are the shared state between all multicore Triangels
uint64_t triangel::address_map_actual_entries = 0;
uint64_t triangel::address_map_rounded_entries = 0;
uint64_t triangel::address_map_num_sets = 0;
uint64_t triangel::size_increment = 0;
uint64_t triangel::max_size = 0;
uint32_t triangel::max_metadata_ways = 0;

uint64_t triangel::all = 0;
uint8_t triangel::current_metadata_ways = 0;
LRURPSetAssocCache<metadata> triangel::markov_table{
    triangel::address_map_rounded_cache_assoc,
    triangel::address_map_rounded_cache_assoc
};
std::vector<duller_entry> triangel::size_duels(NUM_SIZE_DUELERS);
bool triangel::size_duels_initialized = false;
std::mt19937_64 triangel::size_duel_rng{0xC0FFEEULL};
std::mt19937_64 triangel::rng{0x12345678ULL};
std::vector<uint64_t> triangel::set_prefetch_score{};
uint64_t triangel::repartition = 0;
uint64_t triangel::size_duel_tick = 0;
CACHE* triangel::llc_cache = nullptr;

std::vector<triangel::Candidate> triangel::prefetch_candidates(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch,
                                                               access_type type, uint32_t metadata_in, bool allow_training)
{
    (void)metadata_in;

    assert(llc_cache != nullptr);

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
  return metadata_in;
}

void triangel::prefetcher_final_stats()
{
    std::cout << "TRIANGEL Markov table accesses: " << markov_table_accesses << std::endl;
}

void triangel::prefetcher_cycle_operate() {}
