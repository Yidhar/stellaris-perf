#pragma once

#include <cstdint>

// Read-only statistics stellaris_perf.dll publishes in the file mapping
// Local\stellaris_perf_stats_<pid>, for benchmarks (stellaris_bench/) to check that a setting took
// effect and that the fleet manager window really updated. The DLL only writes it; nothing is read
// back, so it is no control channel.
namespace perf {

constexpr uint32_t kSharedMagic = 0x31465250;  // "PRF1"

#pragma pack(push, 8)
struct SharedStats {
    uint32_t magic;
    uint32_t size;  // sizeof(SharedStats), so readers can tell layouts apart
    // applied settings, rewritten by Apply; seq counts the Apply calls
    uint64_t applied_seq;
    int32_t frame_smoothing;
    int32_t opinion_cache;
    int32_t rule_cache;
    int32_t fleet_manager_cache;
    int32_t fleet_manager_reinforce_ms;
    int32_t flag_simd;
    int32_t flag_expiry_skip;
    int32_t fleet_parallel_grain1;  // 1 while the chunk clamp is patched
    // counters, monotonic
    uint64_t fleet_manager_updates;  // CFleetManagerView::Update calls (one per frame while visible)
    uint64_t reinforce_skipped;
    uint64_t reinforce_passed;
    uint64_t fleet_power_hits;    // published every 4096 lookups per thread
    uint64_t fleet_power_misses;
    uint64_t flag_static;     // has_*_flag checks answered by the SSE2 scan (published every 2 s)
    uint64_t flag_dynamic;    // flag@scope checks left to the game
    uint64_t flag_unreadable;
    uint64_t flag_mismatch;   // verify mode: SSE2 answer differed from the game's
    uint64_t expiry_skipped;  // UpdateFlags calls skipped
    uint64_t expiry_passed;
};
#pragma pack(pop)

}  // namespace perf
