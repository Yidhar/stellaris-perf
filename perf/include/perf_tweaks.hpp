#pragma once

#include <cstdint>
#include <string>

namespace perf {

// Log file next to stellaris.exe (stellaris_perf.log).
void Log(const char* fmt, ...);

// Runtime performance tweaks, all off until enabled in stellaris_perf.ini:
//  * frame_smoothing: the engine global g_bFrameSmoothing (the `smooth` console command). When on,
//    the game renders frames in the middle of a tick (smoother UI, a slower simulation).
//  * opinion_cache: memoizes CCountry::CalcOurOpinionOfOther(other, reason = nullptr) per game day.
//  * rule_cache: memoizes CScriptedRule::Evaluate(scope, reason = nullptr) per game day (every scripted
//    CGameRules::* rule), keyed by the rule and the scope's this/from/root/prev objects.
// Caches are direct-mapped thread_local tables (the engine calls both from worker threads). An entry
// is valid for one generation; the generation moves when the game date changes (checked after every
// CGameState::HandleTurnTick) and when a setting changes.
struct Settings {
    int frame_smoothing = -1;  // -1 = leave the game's setting alone, 0 = off, 1 = on
    bool opinion_cache = false;
    // 0 = off, 1 = every rule, 2 = adaptive (only rules measured to gain; see perf_tweaks.cpp)
    int rule_cache = 0;
    // Fleet manager window: caches CFleet::CalcMilitaryPower inside CFleetManagerView::Update for at
    // most one real-time second and one game day (the template list recomputes every ship's stats
    // each frame otherwise).
    bool fleet_manager_cache = true;
    // Fleet manager window: recalculate the ships to reinforce at most every this many ms instead of
    // every 80 ms (0 = the game's own throttle).
    int fleet_manager_reinforce_ms = 1000;
    // has_*_flag: 0 = the game's scan, 1 = SSE2 scan of the same ids (same answer), 2 = verify
    // (the game answers, the SSE2 scan runs too and mismatches are counted)
    int flag_simd = 0;
    // Daily flag expiry: skip containers without a timed flag (the game changes nothing there).
    bool flag_expiry_skip = false;
    // Times every call of the hooked functions whose cache is off, split into the main thread and
    // the others, per game day (reported by StatsLine).
    bool profile = false;
    // Per-rule profile of CScriptedRule::Evaluate: calls / cycles per rule index and path, written to
    // stellaris_perf_rules.csv next to the exe every ~30 s.
    bool rule_profile = false;
    // Modifier graph flush: 0 = the game's parallel job always, 1 = rebuild small, fully known dirty
    // sets serially on the main thread, 2 = verify (both, node contents compared)
    int modifier_flush = 0;
    // Fleet parallel-for (each micro tick): one fleet per chunk instead of fleets / tasks / 3, so a
    // doomstack does not leave the other threads idle (a 5-byte code patch, restored when off)
    bool fleet_parallel_grain1 = true;
    // Profile of scope resolution (CEventTarget::GetScope, CEventScope::Copy) per game day
    bool scope_profile = false;
    int modifier_flush_max = 32;  // largest dirty set taking the serial path
};

// Checks the exe against the SDK and installs the hooks (they pass through while disabled).
bool Install(uintptr_t base);
void Uninstall();
void Apply(const Settings& s);
std::string StatsLine();
// Refreshes the slower counters in the shared statistics (perf_shared.hpp).
void Publish();

} // namespace perf
