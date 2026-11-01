#pragma once

#include <cstdint>
#include <string>

namespace perf {

// Where the log files go: <plugin folder>\logs (stellaris_perf.log, stellaris_perf_rules.csv). Set once by the DLL
// entry, before the first Log; with no folder nothing is written (the game folder is never used).
void SetLogDirectory(const std::wstring& dir);
std::wstring LogDirectory();
void Log(const char* fmt, ...);
struct SharedStats;
SharedStats* Shared();  // the published statistics (perf_shared.hpp), null before Install or after Uninstall

// The identity of a CEventScope for caches keyed by it: the self, from, root and prev slots (object type, id and
// local object pointer each). False when the scope carries parameters or saved event targets (a script may read them)
// or a read faults. The layout is the one the rule cache uses (perf_tweaks.cpp).
struct ScopeKey {
    uint64_t type[4];
    uint64_t local[4];
    uint32_t id[4];
};
bool BuildScopeKey(const void* scope, ScopeKey* key);
// A zeroed per-thread table of the given size, freed by Uninstall once no detour runs any more.
void* RegisterTable(size_t bytes);

// Runtime performance tweaks (config\stellaris_perf.ini of the plugin folder):
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
    int flag_simd = 1;
    // Daily flag expiry: skip containers without a timed flag (the game changes nothing there).
    bool flag_expiry_skip = true;
    // Times every call of the hooked functions whose cache is off, split into the main thread and
    // the others, per game day (reported by StatsLine).
    bool profile = false;
    // Per-rule profile of CScriptedRule::Evaluate: calls / cycles per rule index and path, written to
    // logs\stellaris_perf_rules.csv of the plugin folder every ~30 s.
    bool rule_profile = false;
    // Modifier graph flush: 0 = the game's parallel job always, 1 = rebuild small, fully known dirty
    // sets serially on the main thread, 2 = verify (both, node contents compared)
    int modifier_flush = 0;
    // Fleet parallel-for (each micro tick): one fleet per chunk instead of fleets / tasks / 3, so a
    // doomstack does not leave the other threads idle (a 5-byte code patch, restored when off)
    bool fleet_parallel_grain1 = true;
    // Large-block recycler (perf_alloc.hpp): the engine's big temporary arrays get back the block the same
    // thread freed last, still warm in that core's cache. alloc_cache_mb is what each thread keeps cached.
    bool alloc_cache = true;
    int alloc_cache_mb = 8;
    // Parallel ship update: pass the combat participants table's own SCombatParticipants to CShip::MicroUpdateParallel
    // instead of a deep copy per ship (perf_combat.hpp)
    bool combat_no_copy = true;
    // Colony monthly pass: 1 = the forced modifier refreshes done once at the end of the pass and the manager's flush
    // held during it, 2 = also the colony's node frozen during its pass (perf_colony.hpp); 0 = off
    int colony_monthly_batch = 2;
    // Monthly pop-job assignment once instead of twice: the engine's second UpdatePopJobs call, one more iteration
    // over the first's result, is skipped (perf_jobs.hpp)
    bool pop_jobs_single_pass = true;
    // Daily AI update: the countries processed heaviest first (by the cost measured the day before) (perf_ai.hpp).
    // Measured: no gain, the phase is the heaviest country's own update, which starts first anyway; off. The
    // per-country measurement in the stats line is on regardless.
    bool ai_parallel_lpt = false;
    // Daily AI update: scripted weight factors (ai_weight and the like) memoised per (weight, scope) within one phase
    // (perf_weight.hpp); same results
    bool ai_weight_cache = true;
    // Daily AI update: a parallel-for started inside a country's update runs on that thread instead of forking tasks
    // the other busy workers do not take (perf_ai.hpp); same results. Measured: within noise (the inlined work
    // replaces the wait one for one), so off.
    bool ai_nested_inline = false;
    // Save files: the numbers formatted by plain integer code instead of the C runtime's printf (perf_save.hpp);
    // same text, checked against the runtime at install
    bool save_fast_format = true;
    // Frame rate cap for frames drawn in the middle of a tick (frame smoothing); 0 = the game's own rate (perf_render.hpp)
    int tick_render_fps = 10;
    // Logs how often the engine's frame function runs against the ticks (perf_ui.hpp); measurement only
    bool ui_probe = false;
    // Also apply the tick_render_fps cap to the frames drawn between ticks while the simulation runs (perf_ui.hpp)
    bool render_cap_between_ticks = true;
    // Per-game-day timing of the engine's phases by kind of day, written to logs\stellaris_perf_phases.json
    // (perf_phase.hpp; bench\scripts\phase_report.py prints it); measurement only
    bool phase_profile = false;
    // Extra functions timed by phase_profile: "0xRVA[=name], ..." (perf_phase.hpp)
    std::string phase_extra;
    // Record the call sites of one timed function: "0xRVA" (perf_phase.hpp)
    std::string phase_callers;
    // Profile of scope resolution (CEventTarget::GetScope, CEventScope::Copy) per game day
    bool scope_profile = false;
    int modifier_flush_max = 32;  // largest dirty set taking the serial path
    bool operator==(const Settings&) const = default;
};

// The settings from the ini. Applies them and returns true when the settings in effect changed.
bool SetUserSettings(const Settings& s);

// Checks the exe against the SDK and installs the hooks (they pass through while disabled).
bool Install(uintptr_t base);
void Uninstall();
void Apply(const Settings& s);
std::string StatsLine();
// Refreshes the slower counters in the shared statistics (perf_shared.hpp).
void Publish();

} // namespace perf
