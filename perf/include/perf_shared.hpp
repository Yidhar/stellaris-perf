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
    uint64_t alloc_hits;          // large-block recycler: allocations answered from a cache (published every 2 s)
    uint64_t alloc_misses;
    uint64_t alloc_evictions;
    uint64_t alloc_rejects;
    uint64_t alloc_cached_bytes;
    // the daily AI update's heaviest country (by the cost measured the day before): the thread updating it and
    // whether that update is running right now, for a sampling profiler outside the game (perf_ai.hpp)
    uint32_t ai_heavy_tid;
    uint32_t ai_heavy_active;
    uint32_t ai_heavy_country;  // its index in the CCountryAI array
    uint32_t ai_heavy_pad;
    // the thread serializing a save (CSaveGameTaskManager::CreateMemoryFiles) and whether it is doing so right now
    uint32_t save_tid;
    uint32_t save_active;
    // the thread running the monthly pop-job assignment (NPlanetJobs::UpdatePopJobs, the first pass) and whether it
    // is doing so right now (perf_jobs.hpp)
    uint32_t jobs_tid;
    uint32_t jobs_active;
    // the thread running CGameState::MonthlyUpdate (the month start) and whether it is doing so right now
    uint32_t monthly_tid;
    uint32_t monthly_active;
    // the thread running the modifier manager flush (CModifierNodeManager::Update) and whether one runs right now
    // (perf_nodes.hpp, published only while node_trace is on)
    uint32_t flush_tid;
    uint32_t flush_active;
    // the GUI caches: the applied settings, then monotonic counters
    int32_t ui_power_cache;
    int32_t tooltip_cache;     // 0 off, 1 on, 2 verify; -1 = unavailable (the call sites were not hooked)
    int32_t tooltip_cache_ms;
    int32_t fleet_view_hooked;  // 1 while CFleetViewEntry::OnUpdate is hooked
    uint64_t fleet_view_updates;  // CFleetViewEntry::OnUpdate calls (one per shown entry and frame)
    uint64_t ui_power_hits;       // fleet power asked for by the in-game GUI outside the fleet manager
    uint64_t ui_power_misses;
    uint64_t tooltip_kept;        // provider calls answered from the cache (published every 2 s)
    uint64_t tooltip_built;
    uint64_t tooltip_bypassed;
    uint64_t tooltip_mismatches;  // verify: the provider's answer differed from the cached one
    uint64_t fleet_view_ns;       // time inside CFleetViewEntry::OnUpdate (published every 2 s)
    uint64_t tooltip_build_ns;    // time inside the tooltip providers while the cache or verify is on
    int32_t fleet_upgrade_cache;  // 0 off, 1 on, 2 verify; -1 = unavailable
    int32_t cursor_path_cache;
    uint64_t upgrade_kept;        // the upgrade button's affordability check: answered from the cache / by the engine
    uint64_t upgrade_asked;
    uint64_t upgrade_mismatches;  // verify
    uint64_t upgrade_ns;          // time inside the engine's check
    uint64_t path_kept;           // the cursor's and hover indicator's path check
    uint64_t path_asked;
    uint64_t path_mismatches;
    uint64_t path_ns;
};
#pragma pack(pop)

}  // namespace perf
