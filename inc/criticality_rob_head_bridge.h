#ifndef CRITICALITY_ROB_HEAD_BRIDGE_H
#define CRITICALITY_ROB_HEAD_BRIDGE_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

// Lightweight, header-only bridge that lets core code (the O3_CPU ROB) notify a
// downstream cache prefetcher of the cycle an instruction first reaches the head
// of the ROB (NOT its retirement/commit cycle) without a direct pointer between
// them. Sinks register themselves by cpu index and owner, so several prefetchers
// on one core (e.g. at L1D and L2C) each receive every notification. The registry
// is a C++17 inline variable, so there is exactly one shared instance across the
// core and module translation units.
namespace champsim::criticality_rob_head_bridge
{
using rob_head_sink = std::function<void(uint64_t instr_id, uint64_t rob_head_cycle)>;
using owned_sink = std::pair<const void*, rob_head_sink>;

inline std::unordered_map<uint32_t, std::vector<owned_sink>>& registry()
{
  static std::unordered_map<uint32_t, std::vector<owned_sink>> instance;
  return instance;
}

inline void unregister_rob_head_sink(uint32_t cpu, const void* owner)
{
  auto& reg = registry();
  auto it = reg.find(cpu);
  if (it == reg.end())
    return;

  auto& sinks = it->second;
  sinks.erase(std::remove_if(sinks.begin(), sinks.end(), [owner](const owned_sink& s) { return s.first == owner; }), sinks.end());
  if (sinks.empty())
    reg.erase(it);
}

inline void register_rob_head_sink(uint32_t cpu, const void* owner, rob_head_sink sink)
{
  unregister_rob_head_sink(cpu, owner);
  registry()[cpu].emplace_back(owner, std::move(sink));
}

inline void notify_rob_head(uint32_t cpu, uint64_t instr_id, uint64_t rob_head_cycle)
{
  auto& reg = registry();
  if (reg.empty())
    return;

  if (auto it = reg.find(cpu); it != reg.end()) {
    for (auto& [owner, sink] : it->second) {
      if (sink)
        sink(instr_id, rob_head_cycle);
    }
  }
}

} // namespace champsim::criticality_rob_head_bridge

#endif
