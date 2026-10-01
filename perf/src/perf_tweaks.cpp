#include "perf_tweaks.hpp"
#include "perf_shared.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace perf {

// ---- log ----------------------------------------------------------------------------------

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "stellaris_perf.log");
        g_log = fopen(path, "a");
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

namespace {

using FnHandleTurnTick = void (*)(void* game_state, void* commands);
using FnCalcOpinion = int (*)(const void* country, const void* other, void* reason);
using FnRuleEvaluate = bool (*)(const void* rule, void* scope, void* reason, uint8_t show_tooltip, bool flag);
using FnFleetManagerUpdate = void (*)(void* view);
using FnTemplateGridUpdate = int (*)(void* controller, const void* templates, void* window, void* grid);
using FnHasFlag = bool (*)(const void* trigger, void* scope);
using FnGetFlags = const void* (*)(const void* trigger, void* scope);
using FnUpdateFlags = void (*)(void* flags);
using FnAddInvalid = void (*)(void* mgr, uint32_t node_id, uint32_t category);
using FnManagerUpdate = void (*)(void* mgr);
using FnNodeUpdate = void (*)(void* node);
using FnRandomLogGet = unsigned char* (*)();
using FnGetScope = void* (*)(const void* target, void* out_scope, const void* in_scope, const char* location);
using FnScopeCopy = void (*)(void* dst, const void* src);
using FnGetDynamicFlag = uint16_t* (*)(uint16_t* out, void* scope, const void* target, const void* base, const void* where,
                                       bool log);
using FnFleetPower = int64_t* (*)(const void* fleet, int64_t* out, int type, bool flag);

uintptr_t g_base = 0;
bool g_installed = false;
FnHandleTurnTick g_orig_turn_tick = nullptr;
FnCalcOpinion g_orig_opinion = nullptr;
FnRuleEvaluate g_orig_rule = nullptr;
FnFleetManagerUpdate g_orig_fm_update = nullptr;
FnTemplateGridUpdate g_orig_grid_update = nullptr;
FnHasFlag g_orig_has_flag = nullptr;
FnUpdateFlags g_orig_update_flags = nullptr;
FnAddInvalid g_orig_add_invalid = nullptr;
FnManagerUpdate g_orig_mgr_update = nullptr;
FnGetScope g_orig_get_scope = nullptr;
FnScopeCopy g_orig_scope_copy = nullptr;
FnGetDynamicFlag g_orig_dyn_flag = nullptr;
FnFleetPower g_orig_fleet_power = nullptr;

// Cache generation: bumped when the game date changes (checked after every HandleTurnTick) and
// when a setting changes. Entries of an older generation count as empty, so nothing is cleared.
std::atomic<uint64_t> g_gen{ 1 };
uint32_t g_last_day = 0;  // only the main thread (HandleTurnTick) touches it
// Unload drains the tick detour exactly (it spans a whole tick) and waits for ticks to pass.
std::atomic<int> g_in_tick{ 0 };
std::atomic<uint64_t> g_ticks_done{ 0 };
std::atomic<bool> g_opinion_on{ false };
std::atomic<bool> g_rule_on{ false };
// 0 = off, 1 = every rule, 2 = adaptive (only rules measured to gain from it)
std::atomic<int> g_rule_mode{ 0 };

struct Stats {
    std::atomic<uint64_t> hits{ 0 }, misses{ 0 }, bypass{ 0 };
};
Stats g_opinion_stats, g_rule_stats, g_fm_stats, g_reinforce_stats;
// flags: hits = static checks answered by the SIMD scan, misses = dynamic flag@scope checks left to
// the original, bypass = containers the hook could not read (left to the original)
Stats g_flag_stats;
// expiry: hits = UpdateFlags calls skipped (no timed flag), misses = passed to the original
Stats g_expiry_stats;
std::atomic<uint64_t> g_flag_mismatch{ 0 };

// Counting 30M+ calls a minute on shared atomics would itself cost more than the cache saves:
// each thread counts locally and publishes every 4096 events.
struct LocalCounts {
    uint32_t hits = 0, misses = 0, bypass = 0;
    void Tick(Stats& to) {
        if (hits + misses + bypass < 4096) return;
        to.hits.fetch_add(hits, std::memory_order_relaxed);
        to.misses.fetch_add(misses, std::memory_order_relaxed);
        to.bypass.fetch_add(bypass, std::memory_order_relaxed);
        hits = misses = bypass = 0;
    }
};
thread_local LocalCounts t_opinion_counts, t_rule_counts, t_fm_counts, t_flag_counts, t_expiry_counts;

// ---- profile: what the hooked functions cost, split into the main (tick) thread and the others --

// Only while the hook's cache is off: then every call goes to the original and is timed. Nested
// calls (a rule evaluating rules) count once, in the outermost call.
std::atomic<bool> g_profile{ false };
std::atomic<DWORD> g_main_tid{ 0 };
double g_ms_per_cycle = 0;
std::atomic<uint64_t> g_tick_cycles{ 0 };
uint32_t g_profile_day0 = 0;  // game day when profiling started (0 until a game is loaded)
uint32_t g_scope_day0 = 0;    // same for scope_profile

struct Prof {
    std::atomic<uint64_t> calls[2]{}, cycles[2]{};  // [0] = main thread, [1] = others
    void Reset() {
        for (int i = 0; i < 2; ++i) calls[i] = 0, cycles[i] = 0;
    }
};
Prof g_opinion_prof, g_rule_prof;

struct LocalProf {
    uint64_t calls[2] = {}, cycles[2] = {};
    uint32_t pending = 0;
    int depth = 0;
    void Flush(Prof& to) {
        for (int i = 0; i < 2; ++i) {
            to.calls[i].fetch_add(calls[i], std::memory_order_relaxed);
            to.cycles[i].fetch_add(cycles[i], std::memory_order_relaxed);
            calls[i] = cycles[i] = 0;
        }
        pending = 0;
    }
};
thread_local LocalProf t_opinion_prof, t_rule_prof;

template <class F>
auto Timed(LocalProf& lp, Prof& to, F&& call) {
    if (lp.depth++ != 0) {
        auto r = call();
        --lp.depth;
        return r;
    }
    const uint64_t t0 = __rdtsc();
    auto r = call();
    const uint64_t dt = __rdtsc() - t0;
    --lp.depth;
    const int bucket = GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed) ? 0 : 1;
    ++lp.calls[bucket];
    lp.cycles[bucket] += dt;
    if (++lp.pending >= 1024) lp.Flush(to);
    return r;
}

// A quarter of one core's L2 cache (unified or data), in 64-byte sets, as a power of two in
// [1024, 32768]; 4096 (256 KB) when the size cannot be read.
size_t RuleSetsForL2() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    std::vector<unsigned char> buf(len);
    size_t l2 = 0;
    if (len && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
        for (DWORD off = 0; off < len;) {
            auto* info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
            if (info->Relationship == RelationCache && info->Cache.Level == 2 &&
                (info->Cache.Type == CacheUnified || info->Cache.Type == CacheData)) {
                l2 = info->Cache.CacheSize;
                break;
            }
            off += info->Size;
        }
    }
    size_t sets = 4096;
    if (l2) {
        sets = 1024;
        while (sets * 2 * 64 <= l2 / 4 && sets < 32768) sets *= 2;
    }
    Log("rule cache: L2 %zu KB -> %zu sets (%zu KB per thread)", l2 / 1024, sets, sets * 64 / 1024);
    return sets;
}

double CalibrateMsPerCycle() {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const uint64_t c0 = __rdtsc();
    Sleep(200);
    const uint64_t c1 = __rdtsc();
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / (double)(c1 - c0);
}

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

// Every per-thread table, so Uninstall can free them once no detour runs any more.
std::mutex g_tables_mutex;
std::vector<void*> g_tables;

template <class T>
T* Table(T*& slot, size_t n) {
    if (!slot) {
        slot = (T*)calloc(n, sizeof(T));  // gen 0 = empty; generations start at 1
        std::lock_guard<std::mutex> lock(g_tables_mutex);
        g_tables.push_back(slot);
    }
    return slot;
}

// ---- opinion: direct-mapped per-thread table ----------------------------------------------

struct OpinionEntry {
    uint64_t gen;
    const void* a;
    const void* b;
    int value;
};
constexpr size_t kOpinionSlots = 1u << 14;  // 16K entries, 512 KB per thread
thread_local OpinionEntry* t_opinion = nullptr;

// ---- rules: 4-way set-associative per-thread table ----------------------------------------

// Identity of one scope slot: object type, local object pointer (set for local scopes), and id.
struct ScopeId {
    uint64_t type;
    uint64_t local;
    uint32_t id;
    uint32_t pad;
};
struct RuleKey {
    const void* rule;
    ScopeId self, from, root, prev;
};
// A day has ~100K distinct (rule, scope) keys, so entries keep only a 64-bit hash of the key: four
// 16-byte entries share one cache line. A false hit needs a 64-bit collision within one generation.
struct RuleEntry {
    uint64_t hash;
    uint32_t gen;
    uint8_t value;
    uint8_t pad[3];
};
struct alignas(64) RuleSet {
    RuleEntry way[4];
};
// Sets per thread: a quarter of one core's L2 (64-byte sets), set once at Install from
// GetLogicalProcessorInformationEx, so the table stays cache-resident on any CPU.
size_t g_rule_sets = 4096;
thread_local RuleSet* t_rules = nullptr;

uint64_t HashKey(const RuleKey& k) {
    uint64_t h = (uint64_t)k.rule;
    const ScopeId* slots[4] = { &k.self, &k.from, &k.root, &k.prev };
    for (const ScopeId* p : slots) {
        h = Mix(h, p->type ^ ((uint64_t)p->id << 20));
        h = Mix(h, p->local);
    }
    return h ^ (h >> 33);  // the multiply leaves the low bits weak; they pick the set and way
}

// CEventScope (Windows; same offsets as the Linux build for these members):
//   +0x08 u64 scope type, +0x10 u32 object id, +0x1C local object pointer,
//   +0x30 from, +0x38 root, +0x40 prev (each points at the scope itself when unset),
//   +0x70 CPdxHybridArray<CEventScopeParameter,4> with its size at +0x84.
constexpr size_t kScopeType = 0x08, kScopeId = 0x10, kScopeLocal = 0x1C;
constexpr size_t kScopeFrom = 0x30, kScopeRoot = 0x38, kScopePrev = 0x40, kScopeParamCount = 0x84;

void ReadSlot(const unsigned char* s, ScopeId* out) {
    out->type = *(const uint64_t*)(s + kScopeType);
    out->id = *(const uint32_t*)(s + kScopeId);
    out->local = *(const uint64_t*)(s + kScopeLocal);
}

// Builds the cache key; false when the scope carries parameters (the rule may read them) or a
// read faults.
bool BuildRuleKey(const void* rule, const void* scope, RuleKey* key) {
    memset(key, 0, sizeof(*key));
    key->rule = rule;
    __try {
        const auto* s = (const unsigned char*)scope;
        if (*(const uint32_t*)(s + kScopeParamCount) != 0) {
            return false;
        }
        ReadSlot(s, &key->self);
        const size_t links[3] = { kScopeFrom, kScopeRoot, kScopePrev };
        ScopeId* outs[3] = { &key->from, &key->root, &key->prev };
        for (int i = 0; i < 3; ++i) {
            const auto* p = *(const unsigned char* const*)(s + links[i]);
            if (p && p != s) {
                ReadSlot(p, outs[i]);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t ReadGameDay() {
    __try {
        const uintptr_t gs = *(const uintptr_t*)(g_base + sdk::glob::g_CurrentGameState);
        return gs ? *(const uint32_t*)(gs + 0xC0) / 24 : 0;  // the date is stored in hours
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- fleet manager: fleet military power while the window updates ---------------------------

// CFleetManagerFleetTemplateGridEntry::Update formats every visible template's power each frame,
// and CFleet::CalcMilitaryPower recomputes every ship's full stats for it. Cache it only inside
// CFleetManagerView::Update (the AI and the simulation call the same function), for at most one
// second of real time and never across a game day, keyed also by the fleet's ship array and count
// so a merge or split shows up at once.
std::atomic<bool> g_fm_on{ false };
thread_local int t_in_fm = 0;
thread_local void* t_fm_view = nullptr;

// CFleetManagerView::Update reruns CalcAllShipsToReinforce whenever its inlined
// ShouldUpdateExpensiveThisFrame(8) is true (10 ms of every 80 ms, changed or not), and it is the
// window's biggest cost while the game runs. The flag is stored at the start of Update and read
// after CFleetManagerTemplateGridController::Update, the first call in between: its detour clears
// the flag again unless g_reinforce_ms have passed since the last recalculation.
std::atomic<int> g_reinforce_ms{ 0 };

// Read-only statistics for benchmarks (perf_shared.hpp)
SharedStats* g_shared = nullptr;
HANDLE g_shared_map = nullptr;

void CreateShared() {
    char name[64];
    wsprintfA(name, "Local\\stellaris_perf_stats_%lu", GetCurrentProcessId());
    g_shared_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedStats), name);
    if (!g_shared_map) return;
    g_shared = (SharedStats*)MapViewOfFile(g_shared_map, FILE_MAP_WRITE, 0, 0, sizeof(SharedStats));
    if (!g_shared) return;
    memset(g_shared, 0, sizeof(SharedStats));
    g_shared->size = sizeof(SharedStats);
    g_shared->magic = kSharedMagic;
}

void DestroyShared() {
    SharedStats* shared = g_shared;
    g_shared = nullptr;
    if (shared) UnmapViewOfFile(shared);
    if (g_shared_map) CloseHandle(g_shared_map);
    g_shared_map = nullptr;
}
uint64_t g_last_reinforce = 0;  // GetTickCount64 of the last recalculation let through (main thread)

struct FleetPowerEntry {
    const void* fleet;
    uint64_t ships;  // ship array pointer (+0x320)
    uint64_t stamp;  // generation << 32 | real-time second
    int32_t count;   // ship count (+0x32c)
    int32_t type;
    int64_t value;
    bool flag;
};
constexpr size_t kFleetPowerSlots = 1u << 12;
thread_local FleetPowerEntry* t_fleet_power = nullptr;

// ---- detours ------------------------------------------------------------------------------

// ---- multiplayer guard ---------------------------------------------------------------------------
// The idler's multiplayer flag is set before a game starts (new game, loaded save, joining) and does not
// change while it runs, so it is read once per start, in the two functions that start a game. The
// detour reads it before the original runs, so the start scripts already run with the guard applied.
std::atomic<int> g_in_start{ 0 };
using FnGameStarted = void (*)(void* game_state);
FnGameStarted g_orig_new_game = nullptr;
FnGameStarted g_orig_saved_game = nullptr;

void NewGameStartedDetour(void* game_state) {
    g_in_start.fetch_add(1);
    CheckMultiplayer("new game starting");
    g_orig_new_game(game_state);
    g_in_start.fetch_sub(1);
}

void SavedGameStartedDetour(void* game_state) {
    g_in_start.fetch_add(1);
    CheckMultiplayer("saved game starting");
    g_orig_saved_game(game_state);
    g_in_start.fetch_sub(1);
}

void HandleTurnTickDetour(void* game_state, void* commands) {
    g_in_tick.fetch_add(1);
    g_main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const uint64_t t0 = __rdtsc();
    g_orig_turn_tick(game_state, commands);
    g_tick_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_ticks_done.fetch_add(1);
    g_in_tick.fetch_sub(1);
    const uint32_t day = ReadGameDay();
    // a profile switched on before a game was loaded (DLL injected at the main menu) starts now
    if (g_profile_day0 == 0) g_profile_day0 = day;
    if (g_scope_day0 == 0) g_scope_day0 = day;
    if (day != g_last_day) {
        g_last_day = day;
        g_gen.fetch_add(1, std::memory_order_relaxed);
    }
}

int CalcOpinionDetour(const void* country, const void* other, void* reason) {
    if (!g_opinion_on.load(std::memory_order_relaxed)) {
        if (g_profile.load(std::memory_order_relaxed)) {
            return Timed(t_opinion_prof, g_opinion_prof, [&] { return g_orig_opinion(country, other, reason); });
        }
        return g_orig_opinion(country, other, reason);
    }
    LocalCounts& counts = t_opinion_counts;
    if (reason) {
        ++counts.bypass;
        counts.Tick(g_opinion_stats);
        return g_orig_opinion(country, other, reason);
    }
    const uint64_t gen = g_gen.load(std::memory_order_relaxed);
    OpinionEntry& e = Table(t_opinion, kOpinionSlots)[(Mix((uint64_t)country, (uint64_t)other) >> 40) & (kOpinionSlots - 1)];
    if (e.gen == gen && e.a == country && e.b == other) {
        ++counts.hits;
        counts.Tick(g_opinion_stats);
        return e.value;
    }
    ++counts.misses;
    counts.Tick(g_opinion_stats);
    const int value = g_orig_opinion(country, other, reason);
    e.gen = gen;
    e.a = country;
    e.b = other;
    e.value = value;
    return value;
}

// Per-rule profile (rule_profile=1): calls and cycles per rule index (the int at the start of a
// CScriptedRule) and per path taken: the game's evaluation with the cache off, bypass (reason or
// scope parameters), cache hit, cache miss. It shows which rules are worth caching and whether a
// cache hit (key build + table probe) is actually cheaper than evaluating the rule.
enum RulePath { kRuleOrig, kRuleBypass, kRuleHit, kRuleMiss, kRulePaths };
constexpr int kRuleIndexSlots = 1024;
std::atomic<bool> g_rule_profile{ false };
std::atomic<uint64_t> g_rule_prof_calls[kRulePaths][kRuleIndexSlots];
std::atomic<uint64_t> g_rule_prof_tcalls[kRulePaths][kRuleIndexSlots];  // calls that were timed
std::atomic<uint64_t> g_rule_prof_cycles[kRulePaths][kRuleIndexSlots];

struct LocalRuleProf {
    uint64_t calls[kRulePaths][kRuleIndexSlots];
    uint64_t tcalls[kRulePaths][kRuleIndexSlots];
    uint64_t cycles[kRulePaths][kRuleIndexSlots];
    uint32_t pending;
    uint32_t tick;
};

// Adaptive mode. Per rule index: is the cache used for it. Rules start uncached. Every ~30 s a
// learning window of ~4 s sends every rule through the cache; at its end each rule is judged on
// what was measured since the last decision: the expected cost with the cache,
// h * hit + (1 - h) * miss, against the cost of the game's evaluation (from 1/32 of all calls,
// which always skip the cache, plus the uncached rules' own calls). Enabled when it is below 85%
// of the evaluation cost, disabled again above 95%. 1/8 of the calls are timed with rdtsc.
std::atomic<bool> g_rule_learning{ false };
std::atomic<uint8_t> g_rule_enabled[kRuleIndexSlots];
std::atomic<int> g_rule_enabled_count{ 0 };
std::atomic<double> g_rule_saving_ns_per_s{ 0.0 };
thread_local LocalRuleProf* t_rule_detail = nullptr;

void FlushRuleProf(LocalRuleProf* lp) {
    for (int p = 0; p < kRulePaths; ++p) {
        for (int i = 0; i < kRuleIndexSlots; ++i) {
            if (lp->calls[p][i]) {
                g_rule_prof_calls[p][i].fetch_add(lp->calls[p][i], std::memory_order_relaxed);
                g_rule_prof_tcalls[p][i].fetch_add(lp->tcalls[p][i], std::memory_order_relaxed);
                g_rule_prof_cycles[p][i].fetch_add(lp->cycles[p][i], std::memory_order_relaxed);
                lp->calls[p][i] = lp->tcalls[p][i] = lp->cycles[p][i] = 0;
            }
        }
    }
    lp->pending = 0;
}

bool RuleEvaluateCached(const void* rule, void* scope, void* reason, uint8_t show_tooltip, bool flag, int* path) {
    LocalCounts& counts = t_rule_counts;
    RuleKey key;
    if (reason || !BuildRuleKey(rule, scope, &key)) {
        ++counts.bypass;
        counts.Tick(g_rule_stats);
        *path = kRuleBypass;
        return g_orig_rule(rule, scope, reason, show_tooltip, flag);
    }
    const uint32_t gen = (uint32_t)g_gen.load(std::memory_order_relaxed);
    const uint64_t hash = HashKey(key);
    RuleSet& set = Table(t_rules, g_rule_sets)[(hash >> 7) & (g_rule_sets - 1)];
    RuleEntry* victim = &set.way[hash & 3];  // all four live: evict a pseudo-random way
    for (RuleEntry& e : set.way) {
        if (e.gen != gen) {
            victim = &e;
        } else if (e.hash == hash) {
            ++counts.hits;
            counts.Tick(g_rule_stats);
            *path = kRuleHit;
            return e.value != 0;
        }
    }
    ++counts.misses;
    counts.Tick(g_rule_stats);
    *path = kRuleMiss;
    const bool value = g_orig_rule(rule, scope, reason, show_tooltip, flag);
    // Reuse the generation read before the call: a day change during it makes the entry stale.
    victim->hash = hash;
    victim->gen = gen;
    victim->value = value ? 1 : 0;
    return value;
}

bool RuleEvaluateCore(const void* rule, void* scope, void* reason, uint8_t show_tooltip, bool flag, int* path) {
    if (!g_rule_on.load(std::memory_order_relaxed)) {
        *path = kRuleOrig;
        return g_orig_rule(rule, scope, reason, show_tooltip, flag);
    }
    return RuleEvaluateCached(rule, scope, reason, show_tooltip, flag, path);
}

int RuleIndex(const void* rule) {
    int index = 0;
    __try {
        index = *(const int32_t*)rule;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        index = 0;
    }
    return (index < 0 || index >= kRuleIndexSlots) ? kRuleIndexSlots - 1 : index;
}

bool RuleEvaluateAdaptive(const void* rule, void* scope, void* reason, uint8_t show_tooltip, bool flag) {
    const int index = RuleIndex(rule);
    LocalRuleProf* lp = Table(t_rule_detail, 1);
    const uint32_t tick = ++lp->tick;
    const bool sample_orig = (tick & 31) == 0;
    const bool cached = !sample_orig && (g_rule_learning.load(std::memory_order_relaxed) ||
                                         g_rule_enabled[index].load(std::memory_order_relaxed));
    const bool timed = sample_orig || (tick & 7) == 1;
    int path = kRuleOrig;
    const uint64_t t0 = timed ? __rdtsc() : 0;
    const bool value = cached ? RuleEvaluateCached(rule, scope, reason, show_tooltip, flag, &path)
                              : g_orig_rule(rule, scope, reason, show_tooltip, flag);
    if (timed) {
        ++lp->tcalls[path][index];
        lp->cycles[path][index] += __rdtsc() - t0;
    }
    ++lp->calls[path][index];
    if (++lp->pending >= 8192) FlushRuleProf(lp);
    return value;
}

void DumpRuleProfile();

// Runs every ~2 s on the worker thread (Publish): opens and closes the learning window and
// re-decides after each window.
void RuleAdaptiveStep() {
    static int step = 0;
    static uint64_t prev_calls[kRulePaths][kRuleIndexSlots], prev_tcalls[kRulePaths][kRuleIndexSlots],
        prev_cycles[kRulePaths][kRuleIndexSlots];
    static double prev_time = 0;
    const int phase = step++ % 15;  // 15 steps of ~2 s
    if (phase == 13) {
        g_rule_learning = true;
        return;
    }
    if (phase != 0) return;
    g_rule_learning = false;
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    const double now = (double)c.QuadPart / (double)f.QuadPart;
    const double seconds = prev_time > 0 ? now - prev_time : 0;
    prev_time = now;
    const double ns_per_cycle = g_ms_per_cycle * 1e6;
    int enabled = 0;
    double saving = 0;
    for (int i = 0; i < kRuleIndexSlots; ++i) {
        uint64_t n[kRulePaths], t[kRulePaths], cy[kRulePaths];
        for (int p = 0; p < kRulePaths; ++p) {
            const uint64_t a = g_rule_prof_calls[p][i].load(), b = g_rule_prof_tcalls[p][i].load(),
                           d = g_rule_prof_cycles[p][i].load();
            n[p] = a - prev_calls[p][i];
            t[p] = b - prev_tcalls[p][i];
            cy[p] = d - prev_cycles[p][i];
            prev_calls[p][i] = a;
            prev_tcalls[p][i] = b;
            prev_cycles[p][i] = d;
        }
        bool on = g_rule_enabled[i].load() != 0;
        const uint64_t cached = n[kRuleHit] + n[kRuleMiss];
        if (cached >= 64 && t[kRuleHit] + t[kRuleMiss] >= 16 && t[kRuleOrig] >= 8) {
            const double h = (double)n[kRuleHit] / (double)cached;
            const double orig = cy[kRuleOrig] * ns_per_cycle / t[kRuleOrig];
            const double miss = t[kRuleMiss] ? cy[kRuleMiss] * ns_per_cycle / t[kRuleMiss] : orig * 2;
            const double hit = t[kRuleHit] ? cy[kRuleHit] * ns_per_cycle / t[kRuleHit] : miss;
            const double est = h * hit + (1 - h) * miss;
            if (on && est > 0.95 * orig) on = false;
            else if (!on && est < 0.85 * orig) on = true;
            if (on && seconds > 0) {
                const uint64_t all = n[kRuleOrig] + n[kRuleHit] + n[kRuleMiss];
                saving += (orig - est) * (double)all / seconds;
            }
        }
        g_rule_enabled[i] = on ? 1 : 0;
        enabled += on;
    }
    g_rule_enabled_count = enabled;
    g_rule_saving_ns_per_s = saving;
    if (g_rule_profile.load()) DumpRuleProfile();
}

bool RuleEvaluateDetour(const void* rule, void* scope, void* reason, uint8_t show_tooltip, bool flag) {
    if (g_rule_mode.load(std::memory_order_relaxed) == 2) {
        return RuleEvaluateAdaptive(rule, scope, reason, show_tooltip, flag);
    }
    int path = kRuleOrig;
    if (!g_rule_profile.load(std::memory_order_relaxed)) {
        if (!g_rule_on.load(std::memory_order_relaxed) && g_profile.load(std::memory_order_relaxed)) {
            return Timed(t_rule_prof, g_rule_prof,
                         [&] { return g_orig_rule(rule, scope, reason, show_tooltip, flag); });
        }
        return RuleEvaluateCore(rule, scope, reason, show_tooltip, flag, &path);
    }
    const uint64_t t0 = __rdtsc();
    const bool value = RuleEvaluateCore(rule, scope, reason, show_tooltip, flag, &path);
    const uint64_t dt = __rdtsc() - t0;
    int index = 0;
    __try {
        index = *(const int32_t*)rule;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        index = 0;
    }
    if (index < 0 || index >= kRuleIndexSlots) index = kRuleIndexSlots - 1;
    LocalRuleProf* lp = Table(t_rule_detail, 1);
    ++lp->tcalls[path][index];
    ++lp->calls[path][index];
    lp->cycles[path][index] += dt;
    if (++lp->pending >= 8192) FlushRuleProf(lp);
    return value;
}

void DumpRuleProfile() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "stellaris_perf_rules.csv");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "rule_index,path,calls,timed_calls,cycles,ns_per_timed_call,cache_enabled\n");
    static const char* names[kRulePaths] = { "orig", "bypass", "hit", "miss" };
    for (int p = 0; p < kRulePaths; ++p) {
        for (int i = 0; i < kRuleIndexSlots; ++i) {
            const uint64_t n = g_rule_prof_calls[p][i].load(), t = g_rule_prof_tcalls[p][i].load(),
                           c = g_rule_prof_cycles[p][i].load();
            if (n) fprintf(f, "%d,%s,%llu,%llu,%llu,%.1f,%d\n", i, names[p], (unsigned long long)n,
                           (unsigned long long)t, (unsigned long long)c, t ? c * g_ms_per_cycle * 1e6 / t : 0.0,
                           (int)g_rule_enabled[i].load());
        }
    }
    fclose(f);
}

void FleetManagerUpdateDetour(void* view) {
    ++t_in_fm;
    void* outer = t_fm_view;
    t_fm_view = view;
    if (SharedStats* shared = g_shared) InterlockedIncrement64((volatile LONG64*)&shared->fleet_manager_updates);
    g_orig_fm_update(view);
    t_fm_view = outer;
    --t_in_fm;
}

int TemplateGridUpdateDetour(void* controller, const void* templates, void* window, void* grid) {
    const int ms = g_reinforce_ms.load(std::memory_order_relaxed);
    if (ms > 0 && t_fm_view) {
        // ~12 events a second: count straight into the shared stats
        __try {
            auto* due = (volatile uint8_t*)((char*)t_fm_view + sdk::rt::CFleetManagerView_reinforce_due);
            if (*due) {
                const uint64_t now = GetTickCount64();
                if (now - g_last_reinforce < (uint64_t)ms) {
                    *due = 0;
                    g_reinforce_stats.hits.fetch_add(1, std::memory_order_relaxed);  // skipped
                    if (SharedStats* shared = g_shared) InterlockedIncrement64((volatile LONG64*)&shared->reinforce_skipped);
                } else {
                    g_last_reinforce = now;
                    g_reinforce_stats.misses.fetch_add(1, std::memory_order_relaxed);  // let through
                    if (SharedStats* shared = g_shared) InterlockedIncrement64((volatile LONG64*)&shared->reinforce_passed);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_reinforce_stats.bypass.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return g_orig_grid_update(controller, templates, window, grid);
}

int64_t* FleetPowerDetour(const void* fleet, int64_t* out, int type, bool flag) {
    if (t_in_fm == 0 || !g_fm_on.load(std::memory_order_relaxed)) {
        return g_orig_fleet_power(fleet, out, type, flag);
    }
    LocalCounts& counts = t_fm_counts;
    uint64_t ships;
    int32_t count;
    __try {
        ships = *(const uint64_t*)((const char*)fleet + 0x320);
        count = *(const int32_t*)((const char*)fleet + 0x32c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++counts.bypass;
        counts.Tick(g_fm_stats);
        return g_orig_fleet_power(fleet, out, type, flag);
    }
    const uint64_t stamp = (g_gen.load(std::memory_order_relaxed) << 32) | (uint32_t)(GetTickCount64() / 1000);
    FleetPowerEntry& e = Table(t_fleet_power, kFleetPowerSlots)[(Mix((uint64_t)fleet, (uint64_t)type) >> 40) &
                                                                (kFleetPowerSlots - 1)];
    if (e.fleet == fleet && e.stamp == stamp && e.ships == ships && e.count == count && e.type == type &&
        e.flag == flag) {
        ++counts.hits;
        counts.Tick(g_fm_stats);
        *out = e.value;
        return out;
    }
    ++counts.misses;
    counts.Tick(g_fm_stats);
    int64_t* r = g_orig_fleet_power(fleet, out, type, flag);
    e.fleet = fleet;
    e.ships = ships;
    e.stamp = stamp;
    e.count = count;
    e.type = type;
    e.flag = flag;
    e.value = *r;
    return r;
}

// ---- flags: has_*_flag and daily expiry --------------------------------------------------------

// CPdxIntegerFlags keeps its flag ids (u16) in an unordered array: set appends, remove swaps with
// the last element, and a has_*_flag check scans the whole array (a miss reads every id). The
// engine's loop compares one id per iteration; ContainsU16 answers the same "is id among
// ids[0..n)" with SSE2, 8 ids per compare, and never reads at or past n.
std::atomic<int> g_flag_mode{ 0 };  // 0 = off, 1 = SIMD, 2 = verify (original answers, SIMD compared)
std::atomic<bool> g_expiry_skip{ false };

bool ContainsU16(const uint16_t* ids, int n, uint16_t id) {
    int i = 0;
    if (n >= 8) {
        const __m128i key = _mm_set1_epi16((short)id);
        for (; i + 32 <= n; i += 32) {
            const __m128i a = _mm_cmpeq_epi16(_mm_loadu_si128((const __m128i*)(ids + i)), key);
            const __m128i b = _mm_cmpeq_epi16(_mm_loadu_si128((const __m128i*)(ids + i + 8)), key);
            const __m128i c = _mm_cmpeq_epi16(_mm_loadu_si128((const __m128i*)(ids + i + 16)), key);
            const __m128i d = _mm_cmpeq_epi16(_mm_loadu_si128((const __m128i*)(ids + i + 24)), key);
            if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d)))) return true;
        }
        for (; i + 8 <= n; i += 8) {
            if (_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_loadu_si128((const __m128i*)(ids + i)), key))) return true;
        }
    }
    for (; i < n; ++i) {
        if (ids[i] == id) return true;
    }
    return false;
}

// Any days[i] > -1 (a timed flag) among days[0..n)?
bool AnyTimed(const int32_t* days, int n) {
    int i = 0;
    const __m128i minus1 = _mm_set1_epi32(-1);
    for (; i + 16 <= n; i += 16) {
        const __m128i a = _mm_cmpgt_epi32(_mm_loadu_si128((const __m128i*)(days + i)), minus1);
        const __m128i b = _mm_cmpgt_epi32(_mm_loadu_si128((const __m128i*)(days + i + 4)), minus1);
        const __m128i c = _mm_cmpgt_epi32(_mm_loadu_si128((const __m128i*)(days + i + 8)), minus1);
        const __m128i d = _mm_cmpgt_epi32(_mm_loadu_si128((const __m128i*)(days + i + 12)), minus1);
        if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, d)))) return true;
    }
    for (; i < n; ++i) {
        if (days[i] > -1) return true;
    }
    return false;
}

// Reads the static flag's container the way the original does: 0 = answered in *found,
// 1 = dynamic flag, 2 = unreadable (both left to the original).
int StaticFlagLookup(const void* trigger, void* scope, bool* found) {
    const auto* t = (const unsigned char*)trigger;
    __try {
        if (*(const uint64_t*)(t + sdk::rt::CHasFlagTrigger_dynamic_size) != 0) return 1;
        const uint16_t id = *(const uint16_t*)(t + sdk::rt::CHasFlagTrigger_flag);
        const auto get_flags = *(const FnGetFlags*)(*(const uintptr_t*)t + sdk::rt::CHasFlagTrigger_vt_GetFlags);
        const auto* flags = (const unsigned char*)get_flags(trigger, scope);
        if (!flags) return 2;
        const uint16_t* ids = *(const uint16_t* const*)(flags + sdk::rt::CPdxIntegerFlags_ids);
        const int n = *(const int32_t*)(flags + sdk::rt::CPdxIntegerFlags_count);
        *found = n > 0 && ContainsU16(ids, n, id);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}

bool HasFlagDetour(const void* trigger, void* scope) {
    const int mode = g_flag_mode.load(std::memory_order_relaxed);
    if (mode == 0) return g_orig_has_flag(trigger, scope);
    LocalCounts& counts = t_flag_counts;
    bool found = false;
    const int r = StaticFlagLookup(trigger, scope, &found);
    if (r != 0) {
        ++(r == 1 ? counts.misses : counts.bypass);
        counts.Tick(g_flag_stats);
        return g_orig_has_flag(trigger, scope);
    }
    ++counts.hits;
    counts.Tick(g_flag_stats);
    if (mode == 2) {
        const bool original = g_orig_has_flag(trigger, scope);
        if (original != found) g_flag_mismatch.fetch_add(1, std::memory_order_relaxed);
        return original;
    }
    return found;
}

// The original only touches entries with days > -1 (decrements them, removes those reaching 0);
// with none it changes nothing.
void UpdateFlagsDetour(void* flags) {
    if (!g_expiry_skip.load(std::memory_order_relaxed)) return g_orig_update_flags(flags);
    LocalCounts& counts = t_expiry_counts;
    bool timed = true;
    __try {
        const auto* f = (const unsigned char*)flags;
        const int n = *(const int32_t*)(f + sdk::rt::CPdxIntegerFlags_count);
        const int32_t* days = *(const int32_t* const*)(f + sdk::rt::CPdxIntegerFlags_days);
        timed = n > 0 && AnyTimed(days, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        timed = true;
    }
    if (!timed) {
        ++counts.hits;
        counts.Tick(g_expiry_stats);
        return;
    }
    ++counts.misses;
    counts.Tick(g_expiry_stats);
    g_orig_update_flags(flags);
}

// ---- modifier graph flush: serial fast path for small flushes ----------------------------------

// CModifierNodeManager::Update (the flush after every effect block, list effect, command and phase)
// runs a ~100-task parallel graph job over the dirty modifier nodes even when one node is dirty; the
// main thread mostly spin-waits for it. When the dirty set is small and fully known, this rebuilds
// those nodes directly on the main thread with CModifierNodeBase::Update (the engine's on-demand
// rebuild, which updates dirty parents first), under the same manager state as the engine's flush
// (busy set, has-invalid cleared, masked-invalidate cleared, RNG forbidden, random-log config |= 2).
// The game is deterministic across thread counts and schedules, so node contents cannot depend on
// the rebuild order; a serial order is one valid schedule.
//
// Knowing the dirty set: every invalidation ends in AddInvalid (inlined Invalidate tail, the only
// writer of has-invalid). Main-thread calls are recorded in a buffer. Worker-thread calls cannot be
// ordered against the flush cheaply, so any since the last full flush forces the full path: workers
// bump g_winv_begin before and g_winv_end after the engine's insert, and the full flush remembers
// g_winv_end as the base only if no insert was in flight (begin == end) when it started.
constexpr int kInvCap = 256;
constexpr uint64_t kNoBase = ~0ull;
constexpr ptrdiff_t kSlotStride = 0x18;  // `lea rdx, [rax + rax*2]` ... `[rcx + rdx*8]` (anchors.py)
std::atomic<int> g_flush_mode{ 0 };      // 0 = off, 1 = fast path, 2 = verify (engine rebuilds too, compared)
std::atomic<int> g_flush_max{ 32 };
uint32_t g_inv_ids[kInvCap];              // main thread only
int g_inv_n = 0;
bool g_inv_overflow = false;
std::atomic<uint64_t> g_winv_begin{ 0 }, g_winv_end{ 0 };
uint64_t g_flush_base = kNoBase;          // main thread only
std::atomic<uint64_t> g_flush_fast{ 0 }, g_flush_full{ 0 }, g_flush_nodes{ 0 }, g_flush_mismatch{ 0 };
std::atomic<uint64_t> g_flush_fast_cycles{ 0 }, g_flush_full_cycles{ 0 };
std::atomic<uint64_t> g_flush_why[4];     // full-path reasons: worker invalidations, too many, overflow, no base
std::atomic<uint64_t> g_flush_hist[7];    // fast-path sizes: 1, 2, 3-4, 5-8, 9-16, 17-32, 33+

void AddInvalidDetour(void* mgr, uint32_t node_id, uint32_t category) {
    if (g_flush_mode.load(std::memory_order_relaxed) == 0) return g_orig_add_invalid(mgr, node_id, category);
    if (GetCurrentThreadId() != g_main_tid.load(std::memory_order_relaxed)) {
        g_winv_begin.fetch_add(1);
        g_orig_add_invalid(mgr, node_id, category);
        g_winv_end.fetch_add(1);
        return;
    }
    g_orig_add_invalid(mgr, node_id, category);
    if (g_inv_n < kInvCap) g_inv_ids[g_inv_n++] = node_id;
    else g_inv_overflow = true;
}

// Category order. The engine's flush rebuilds category by category in dependency order (a node's
// calc may read nodes of the categories it depends on); the serial path follows the same order.
// Each node points at its node type (+0): the category id at +8 and the categories it depends on
// as an int range [+0x10, +0x18). Read from live nodes once per manager and checked (sane ranges,
// ids < 64, no cycle); if the check fails the fast path stays off. (Offsets verified against live
// memory in 4.5.1, not anchored in code: the check is the guard.)
constexpr int kMaxCat = 64;
constexpr ptrdiff_t kTypeCategory = 0x8, kTypeDepsBegin = 0x10, kTypeDepsEnd = 0x18;
int8_t g_cat_rank[kMaxCat];
const void* g_rank_mgr = nullptr;
bool g_rank_ok = false;
std::atomic<uint64_t> g_flush_mismatch_cat[kMaxCat];
std::atomic<uint64_t> g_flush_nodes_cat[kMaxCat];

int NodeCategory(const void* node) {
    const unsigned char* type = *(unsigned char* const*)node;
    return (int)*(const uint32_t*)(type + kTypeCategory);
}

// Reads each category's dependency list from the live nodes (SEH-guarded; plain arrays only).
bool ReadCategoryDeps(const unsigned char* mgr, int8_t (*deps)[kMaxCat], int* ndeps) {
    bool seen[kMaxCat] = {};
    __try {
        const uint32_t count = *(const uint32_t*)(mgr + sdk::rt::CModifierNodeManager_slot_count);
        const unsigned char* slots = *(unsigned char* const*)(mgr + sdk::rt::CModifierNodeManager_slots);
        const unsigned char* last_type = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            const unsigned char* node = *(unsigned char* const*)(slots + i * kSlotStride + sdk::rt::CModifierNodeManager_slot_node);
            if (!node) continue;
            const unsigned char* type = *(unsigned char* const*)node;
            if (type == last_type) continue;
            last_type = type;
            const uint32_t cat = *(const uint32_t*)(type + kTypeCategory);
            if (cat >= kMaxCat) return false;
            if (seen[cat]) continue;
            seen[cat] = true;
            const int32_t* b = *(const int32_t* const*)(type + kTypeDepsBegin);
            const int32_t* e = *(const int32_t* const*)(type + kTypeDepsEnd);
            if ((b == nullptr) != (e == nullptr) || e < b || e - b > kMaxCat) return false;
            for (const int32_t* d = b; d < e; ++d) {
                if (*d < 0 || *d >= kMaxCat) return false;
                deps[cat][ndeps[cat]++] = (int8_t)*d;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool BuildCategoryRanks(const unsigned char* mgr) {
    static int8_t deps[kMaxCat][kMaxCat];
    int ndeps[kMaxCat] = {};
    if (!ReadCategoryDeps(mgr, deps, ndeps)) return false;
    // rank = longest dependency chain; a cycle fails the check
    int8_t state[kMaxCat] = {};  // 0 new, 1 on stack, 2 done
    int8_t rank[kMaxCat] = {};
    for (int root = 0; root < kMaxCat; ++root) {
        if (state[root]) continue;
        std::vector<std::pair<int, int>> stack{ { root, 0 } };
        state[root] = 1;
        while (!stack.empty()) {
            auto& [c, k] = stack.back();
            if (k < ndeps[c]) {
                const int d = deps[c][k++];
                if (state[d] == 1) return false;
                if (state[d] == 0) {
                    state[d] = 1;
                    stack.push_back({ d, 0 });
                }
                continue;
            }
            int r = 0;
            for (int i = 0; i < ndeps[c]; ++i) r = r > rank[deps[c][i]] + 1 ? r : rank[deps[c][i]] + 1;
            rank[c] = (int8_t)r;
            state[c] = 2;
            stack.pop_back();
        }
    }
    memcpy(g_cat_rank, rank, sizeof(rank));
    return true;
}

void* NodeOf(const unsigned char* mgr, uint32_t id) {
    const uint32_t index = id & 0xFFFFF;
    if (index >= *(const uint32_t*)(mgr + sdk::rt::CModifierNodeManager_slot_count)) return nullptr;
    const unsigned char* slot = *(unsigned char* const*)(mgr + sdk::rt::CModifierNodeManager_slots) + index * kSlotStride;
    if (*(const uint32_t*)slot != id) return nullptr;
    return *(void* const*)(slot + sdk::rt::CModifierNodeManager_slot_node);
}

// Node content for the verify mode: entries (value, type) and parent records (mult, name,
// category). A parent record's clone pointer is left out: every rebuild clones anew.
uint64_t NodeHash(const unsigned char* node) {
    const unsigned char* m = node + sdk::rt::CModifierNode_modifier;
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
    const int n = *(const int32_t*)(m + sdk::rt::CModifier_entry_count);
    const unsigned char* e = *(unsigned char* const*)(m + sdk::rt::CModifier_entries);
    mix((uint64_t)n);
    for (int i = 0; i < n; ++i) mix(*(const uint64_t*)(e + i * 16)), mix(*(const uint32_t*)(e + i * 16 + 8));
    const int pn = *(const int32_t*)(m + sdk::rt::CModifier_parent_count);
    const unsigned char* pr = *(unsigned char* const*)(m + sdk::rt::CModifier_parents);
    mix((uint64_t)pn);
    for (int i = 0; i < pn; ++i) {
        mix(*(const uint64_t*)(pr + i * 0x20 + 8));
        mix(*(const uint64_t*)(pr + i * 0x20 + 0x10));
        mix(*(const uint32_t*)(pr + i * 0x20 + 0x18));
    }
    return h;
}

void FullFlush(void* mgr, int why) {
    if (why >= 0) g_flush_why[why].fetch_add(1, std::memory_order_relaxed);
    // everything recorded so far is in the engine's sets and drained by this flush
    g_inv_n = 0;
    g_inv_overflow = false;
    const uint64_t begin = g_winv_begin.load(), end = g_winv_end.load();
    g_flush_base = begin == end ? end : kNoBase;
    const uint64_t t0 = __rdtsc();
    g_orig_mgr_update(mgr);
    g_flush_full_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_flush_full.fetch_add(1, std::memory_order_relaxed);
}

void ManagerUpdateDetour(void* mgr) {
    const int mode = g_flush_mode.load(std::memory_order_relaxed);
    if (mode == 0 || GetCurrentThreadId() != g_main_tid.load(std::memory_order_relaxed)) return g_orig_mgr_update(mgr);
    auto* m = (unsigned char*)mgr;
    // batched, busy or nothing invalid: the engine returns without collecting; keep what we recorded
    if (m[sdk::rt::CModifierNodeManager_batch] || m[sdk::rt::CModifierNodeManager_busy] ||
        !m[sdk::rt::CModifierNodeManager_has_invalid]) {
        return g_orig_mgr_update(mgr);
    }
    if (g_flush_base == kNoBase) return FullFlush(mgr, 3);
    if (g_winv_begin.load() != g_flush_base) return FullFlush(mgr, 0);
    if (g_inv_overflow) return FullFlush(mgr, 2);
    if (g_rank_mgr != mgr) {
        g_rank_mgr = mgr;
        g_rank_ok = BuildCategoryRanks(m);
        Log("modifier flush: category order %s", g_rank_ok ? "built" : "FAILED the check; fast path off");
    }
    if (!g_rank_ok) return FullFlush(mgr, 3);
    const int n = g_inv_n;
    if (n == 0) return FullFlush(mgr, 3);  // has-invalid set but nothing recorded: an untracked source
    if (n > g_flush_max.load(std::memory_order_relaxed)) return FullFlush(mgr, 1);

    if (mode == 3) {
        // Control for the verify mode: the engine's flush builds the recorded nodes, then builds them
        // again after they are marked dirty once more. Mismatches here mean a node's rebuild is not
        // repeatable (its calc reads state that the rebuild itself changes), so verify-mode
        // mismatches of that category say nothing about the serial path.
        void* nodes[kInvCap];
        uint32_t node_ids[kInvCap];
        int k = 0;
        for (int i = 0; i < n; ++i) {
            void* node = NodeOf(m, g_inv_ids[i]);
            bool seen = false;
            for (int j = 0; j < k && !seen; ++j) seen = nodes[j] == node;
            if (node && !seen && (((unsigned char*)node)[sdk::rt::CModifierNode_dirty] & 1)) {
                node_ids[k] = g_inv_ids[i];
                nodes[k++] = node;
            }
        }
        FullFlush(mgr, -1);
        uint64_t hashes[kInvCap];
        for (int i = 0; i < k; ++i) hashes[i] = NodeHash((unsigned char*)nodes[i]);
        for (int i = 0; i < k; ++i) {
            // dirty again and back into the engine's invalid set, as Invalidate does (the first
            // flush drained the set; a dirty node missing from it is a state the engine never has)
            InterlockedExchange8((volatile char*)&((unsigned char*)nodes[i])[sdk::rt::CModifierNode_dirty], 1);
            g_orig_add_invalid(mgr, node_ids[i], (uint32_t)NodeCategory(nodes[i]));
        }
        FullFlush(mgr, -1);
        for (int i = 0; i < k; ++i) {
            const int cat = NodeCategory(nodes[i]);
            g_flush_nodes_cat[cat].fetch_add(1, std::memory_order_relaxed);
            if (NodeHash((unsigned char*)nodes[i]) != hashes[i]) {
                g_flush_mismatch.fetch_add(1);
                g_flush_mismatch_cat[cat].fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    const uint64_t t0 = __rdtsc();
    uint32_t ids[kInvCap];
    memcpy(ids, g_inv_ids, n * sizeof(uint32_t));
    g_inv_n = 0;
    InterlockedExchange8((volatile char*)&m[sdk::rt::CModifierNodeManager_busy], 1);
    InterlockedExchange8((volatile char*)&m[sdk::rt::CModifierNodeManager_has_invalid], 0);
    const unsigned char masked = m[sdk::rt::CModifierNodeManager_masked];
    m[sdk::rt::CModifierNodeManager_masked] = 0;
    auto* forbidden = (volatile unsigned char*)(g_base + sdk::glob::CRandom_Forbidden);
    const unsigned char forbidden_was = *forbidden;
    *forbidden = 1;
    unsigned char* rlog = ((FnRandomLogGet)(g_base + sdk::fn::CRandomLog_Get))();
    const int32_t rlog_was = *(int32_t*)(rlog + sdk::rt::CRandomLog_config);
    *(int32_t*)(rlog + sdk::rt::CRandomLog_config) = rlog_was | 2;

    // dirty recorded nodes, deduplicated, in category dependency order (insertion sort, n <= max)
    void* nodes[kInvCap];
    uint32_t node_ids[kInvCap];
    int ranks[kInvCap];
    int rebuilt = 0;
    for (int i = 0; i < n; ++i) {
        void* node = NodeOf(m, ids[i]);
        if (!node || !(((unsigned char*)node)[sdk::rt::CModifierNode_dirty] & 1)) continue;
        bool seen = false;
        for (int j = 0; j < rebuilt && !seen; ++j) seen = nodes[j] == node;
        if (seen) continue;
        const int r = g_cat_rank[NodeCategory(node)];
        int j = rebuilt++;
        for (; j > 0 && ranks[j - 1] > r; --j) nodes[j] = nodes[j - 1], ranks[j] = ranks[j - 1], node_ids[j] = node_ids[j - 1];
        nodes[j] = node;
        ranks[j] = r;
        node_ids[j] = ids[i];
    }
    for (int i = 0; i < rebuilt; ++i) ((FnNodeUpdate)(g_base + sdk::fn::CModifierNodeBase_Update))(nodes[i]);

    *(int32_t*)(rlog + sdk::rt::CRandomLog_config) = rlog_was;
    *forbidden = forbidden_was;
    m[sdk::rt::CModifierNodeManager_masked] = masked;
    InterlockedExchange8((volatile char*)&m[sdk::rt::CModifierNodeManager_busy], 0);
    g_flush_fast_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_flush_fast.fetch_add(1, std::memory_order_relaxed);
    g_flush_nodes.fetch_add(rebuilt, std::memory_order_relaxed);
    const int bucket = n <= 1 ? 0 : n <= 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : n <= 32 ? 5 : 6;
    g_flush_hist[bucket].fetch_add(1, std::memory_order_relaxed);

    if (mode == 2 && rebuilt) {
        // Verify: hash what the fast path built, mark the nodes dirty again (their ids are still in
        // the engine's per-thread sets) and let the engine's own flush rebuild them; compare.
        uint64_t hashes[kInvCap];
        for (int i = 0; i < rebuilt; ++i) hashes[i] = NodeHash((unsigned char*)nodes[i]);
        for (int i = 0; i < rebuilt; ++i) {
            // dirty again and back into the engine's invalid set, as Invalidate does (the first
            // flush drained the set; a dirty node missing from it is a state the engine never has)
            InterlockedExchange8((volatile char*)&((unsigned char*)nodes[i])[sdk::rt::CModifierNode_dirty], 1);
            g_orig_add_invalid(mgr, node_ids[i], (uint32_t)NodeCategory(nodes[i]));
        }
        FullFlush(mgr, -1);
        for (int i = 0; i < rebuilt; ++i) {
            const int cat = NodeCategory(nodes[i]);
            g_flush_nodes_cat[cat].fetch_add(1, std::memory_order_relaxed);
            if (NodeHash((unsigned char*)nodes[i]) != hashes[i]) {
                g_flush_mismatch.fetch_add(1);
                g_flush_mismatch_cat[cat].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ---- fleet parallel-for: one fleet per chunk ------------------------------------------------

// Each micro tick CFleet::MicroUpdateParallel runs as a parallel-for over every fleet: tasks =
// threads + 1, and each task takes chunks of max(1, fleets / tasks / 3) fleets from a shared atomic
// index. Fleets differ enormously (one ship vs. hundreds in a doomstack), and fleets created
// together sit next to each other, so one chunk can hold a whole battle while the other threads
// idle and the main thread waits. NOP-ing the inlined `cmp edx, eax; cmovg eax, edx` leaves the
// chunk at 1, so idle threads keep taking single fleets.
// Same results: the partition only decides which thread updates which fleet; the phase runs with
// the RNG forbidden, fleets are updated independently, and the engine already partitions by each
// machine's thread count while multiplayer stays in sync.
constexpr uint8_t kGrainClamp[5] = { 0x3B, 0xD0, 0x0F, 0x4F, 0xC2 };  // cmp edx, eax; cmovg eax, edx
constexpr uint8_t kGrainNops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
bool g_grain_patched = false;

// Replaces n bytes at addr (all inside one aligned 8-byte word) with one atomic 8-byte write,
// after checking they still hold `from`.
bool PatchCode(uintptr_t addr, const uint8_t* from, const uint8_t* to, size_t n) {
    const uintptr_t word = addr & ~(uintptr_t)7;
    const size_t off = addr - word;
    if (off + n > 8) return false;
    DWORD old;
    if (!VirtualProtect((void*)word, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    volatile LONG64* p = (volatile LONG64*)word;
    const LONG64 cur = *p;
    uint8_t b[8];
    memcpy(b, (const void*)&cur, 8);
    bool ok = memcmp(b + off, from, n) == 0;
    if (ok) {
        memcpy(b + off, to, n);
        LONG64 next;
        memcpy(&next, b, 8);
        ok = InterlockedCompareExchange64(p, next, cur) == cur;
    }
    DWORD ignored;
    VirtualProtect((void*)word, 8, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)word, 8);
    return ok;
}

void SetFleetGrain1(bool on) {
    if (!g_base || on == g_grain_patched) return;
    const uintptr_t site = g_base + sdk::glob::UpdateShipParallel_GrainClamp;
    if (PatchCode(site, on ? kGrainClamp : kGrainNops, on ? kGrainNops : kGrainClamp, 5)) {
        g_grain_patched = on;
        Log("fleet parallel-for chunk clamp %s", on ? "patched (1 fleet per chunk)" : "restored");
    } else {
        Log("fleet parallel-for chunk clamp: bytes at 0x%llX did not match, left alone", (unsigned long long)site);
    }
}

// ---- scope resolution profile (scope_profile=1) -----------------------------------------------

// CEventTarget::GetScope resolves every scope switch (owner, from, prev, event_target:x, ...): it
// copy-constructs the result from the input scope, and the copy deep-copies the chain base's event
// target container (local event targets + local_ variables) when there is one. Counted per game day,
// split into event_target resolutions vs other switches, chain base with / without a container,
// main thread vs others (outermost call timed, nested segments included). CEventScope::Copy is
// counted separately: deep copies (source has a container) vs plain ones.
std::atomic<bool> g_scope_profile{ false };
// [event_target][has container][main]
std::atomic<uint64_t> g_gs_calls[2][2][2], g_gs_cycles[2][2][2];
std::atomic<uint64_t> g_copy_calls[2][2], g_copy_cycles[2][2];  // [deep][main]
std::atomic<uint64_t> g_dyn_calls[2], g_dyn_cycles[2], g_dyn_miss[2];  // [main]; miss = name not in the table
thread_local int t_gs_depth = 0;

struct LocalScopeProf {
    uint64_t gs_calls[2][2][2] = {}, gs_cycles[2][2][2] = {}, copy_calls[2][2] = {}, copy_cycles[2][2] = {};
    uint32_t pending = 0;
    void Tick() {
        if (++pending < 4096) return;
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b) {
                for (int c = 0; c < 2; ++c) {
                    g_gs_calls[a][b][c].fetch_add(gs_calls[a][b][c], std::memory_order_relaxed);
                    g_gs_cycles[a][b][c].fetch_add(gs_cycles[a][b][c], std::memory_order_relaxed);
                    gs_calls[a][b][c] = gs_cycles[a][b][c] = 0;
                }
                g_copy_calls[a][b].fetch_add(copy_calls[a][b], std::memory_order_relaxed);
                g_copy_cycles[a][b].fetch_add(copy_cycles[a][b], std::memory_order_relaxed);
                copy_calls[a][b] = copy_cycles[a][b] = 0;
            }
        pending = 0;
    }
};
thread_local LocalScopeProf t_scope_prof;

// Does the chain base of `scope` (follow `from` until it points at itself) hold a container?
bool ChainBaseHasContainer(const unsigned char* scope) {
    __try {
        for (int i = 0; i < 64 && scope; ++i) {
            const unsigned char* from = *(unsigned char* const*)(scope + sdk::rt::CEventScope_from);
            if (!from || from == scope) return *(void* const*)(scope + sdk::rt::CEventScope_event_targets) != nullptr;
            scope = from;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void* GetScopeDetour(const void* target, void* out_scope, const void* in_scope, const char* location) {
    if (!g_scope_profile.load(std::memory_order_relaxed) || t_gs_depth++ != 0) {
        void* r = g_orig_get_scope(target, out_scope, in_scope, location);
        if (g_scope_profile.load(std::memory_order_relaxed)) --t_gs_depth;
        return r;
    }
    int et = 0;
    __try {
        et = ((const unsigned char*)target)[sdk::rt::CEventTarget_is_event_target] ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const int container = ChainBaseHasContainer((const unsigned char*)in_scope) ? 1 : 0;
    const int main = GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed) ? 1 : 0;
    const uint64_t t0 = __rdtsc();
    void* r = g_orig_get_scope(target, out_scope, in_scope, location);
    LocalScopeProf& lp = t_scope_prof;
    ++lp.gs_calls[et][container][main];
    lp.gs_cycles[et][container][main] += __rdtsc() - t0;
    lp.Tick();
    --t_gs_depth;
    return r;
}

uint16_t* GetDynamicFlagDetour(uint16_t* out, void* scope, const void* target, const void* base, const void* where,
                               bool log) {
    if (!g_scope_profile.load(std::memory_order_relaxed)) return g_orig_dyn_flag(out, scope, target, base, where, log);
    const int main = GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed) ? 1 : 0;
    const uint64_t t0 = __rdtsc();
    uint16_t* r = g_orig_dyn_flag(out, scope, target, base, where, log);
    g_dyn_cycles[main].fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_dyn_calls[main].fetch_add(1, std::memory_order_relaxed);
    if (r && *r == 0xFFFF) g_dyn_miss[main].fetch_add(1, std::memory_order_relaxed);
    return r;
}

void ScopeCopyDetour(void* dst, const void* src) {
    if (!g_scope_profile.load(std::memory_order_relaxed)) return g_orig_scope_copy(dst, src);
    int deep = 0;
    __try {
        deep = *(void* const*)((const unsigned char*)src + sdk::rt::CEventScope_event_targets) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const int main = GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed) ? 1 : 0;
    const uint64_t t0 = __rdtsc();
    g_orig_scope_copy(dst, src);
    LocalScopeProf& lp = t_scope_prof;
    ++lp.copy_calls[deep][main];
    lp.copy_cycles[deep][main] += __rdtsc() - t0;
    lp.Tick();
}

std::string ScopeStats() {
    if (!g_scope_profile.load()) return "scope_profile=0";
    const double days = (double)(ReadGameDay() - g_scope_day0);
    if (days <= 0) return "scope_profile=1 (no day yet)";
    const double k = g_ms_per_cycle / days;
    std::string out = "scope_profile=1 per day over " + std::to_string((int)days) + " days:";
    static const char* kind[2] = { "switch", "event_target" };
    static const char* base[2] = { "", "+container" };
    char buf[160];
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            snprintf(buf, sizeof(buf), " %s%s main %.0f calls %.2f ms / other %.0f calls %.2f ms;", kind[a], base[b],
                     g_gs_calls[a][b][1].load() / days, g_gs_cycles[a][b][1].load() * k,
                     g_gs_calls[a][b][0].load() / days, g_gs_cycles[a][b][0].load() * k);
            out += buf;
        }
    snprintf(buf, sizeof(buf), " dynamic name@target main %.0f calls (%.0f unknown) %.2f ms / other %.0f calls (%.0f unknown) %.2f ms;",
             g_dyn_calls[1].load() / days, g_dyn_miss[1].load() / days, g_dyn_cycles[1].load() * k,
             g_dyn_calls[0].load() / days, g_dyn_miss[0].load() / days, g_dyn_cycles[0].load() * k);
    out += buf;
    static const char* copy[2] = { "copy", "deep copy" };
    for (int a = 0; a < 2; ++a) {
        snprintf(buf, sizeof(buf), " %s main %.0f calls %.2f ms / other %.0f calls %.2f ms;", copy[a],
                 g_copy_calls[a][1].load() / days, g_copy_cycles[a][1].load() * k, g_copy_calls[a][0].load() / days,
                 g_copy_cycles[a][0].load() * k);
        out += buf;
    }
    return out;
}

bool Hook(uintptr_t target, void* detour, void** original, const char* name) {
    if (MH_CreateHook((LPVOID)target, detour, original) != MH_OK || MH_EnableHook((LPVOID)target) != MH_OK) {
        Log("could not hook %s at 0x%llX", name, (unsigned long long)target);
        return false;
    }
    Log("hooked %s at 0x%llX", name, (unsigned long long)target);
    return true;
}

bool ExeMatchesSdk(uintptr_t base) {
    auto dos = (PIMAGE_DOS_HEADER)base;
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    if (stamp != sdk::kExeTimestamp) {
        Log("exe TimeDateStamp 0x%08X does not match the SDK (0x%08X); not installing. "
            "Rebuild after running tools/sdk_dumper/dump.py.", stamp, sdk::kExeTimestamp);
        return false;
    }
    return true;
}

} // namespace

bool Install(uintptr_t base) {
    if (!ExeMatchesSdk(base)) return false;
    g_base = base;
    CreateShared();
    g_ms_per_cycle = CalibrateMsPerCycle();
    g_rule_sets = RuleSetsForL2();
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed (%d)", (int)st);
        return false;
    }
    bool ok = Hook(base + sdk::fn::CGameState_HandleTurnTick, (void*)&HandleTurnTickDetour,
                   (void**)&g_orig_turn_tick, "CGameState::HandleTurnTick");
    ok = Hook(base + sdk::fn::CCountry_CalcOurOpinionOfOther, (void*)&CalcOpinionDetour,
              (void**)&g_orig_opinion, "CCountry::CalcOurOpinionOfOther") && ok;
    ok = Hook(base + sdk::fn::CScriptedRule_Evaluate, (void*)&RuleEvaluateDetour,
              (void**)&g_orig_rule, "CScriptedRule::Evaluate") && ok;
    ok = Hook(base + sdk::fn::CFleetManagerView_Update, (void*)&FleetManagerUpdateDetour,
              (void**)&g_orig_fm_update, "CFleetManagerView::Update") && ok;
    ok = Hook(base + sdk::fn::CFleet_CalcMilitaryPower, (void*)&FleetPowerDetour,
              (void**)&g_orig_fleet_power, "CFleet::CalcMilitaryPower") && ok;
    ok = Hook(base + sdk::fn::CFleetManagerTemplateGridController_Update, (void*)&TemplateGridUpdateDetour,
              (void**)&g_orig_grid_update, "CFleetManagerTemplateGridController::Update") && ok;
    ok = Hook(base + sdk::fn::CHasFlagTrigger_ActualEvaluate, (void*)&HasFlagDetour, (void**)&g_orig_has_flag,
              "CHasFlagTrigger::ActualEvaluate") && ok;
    ok = Hook(base + sdk::fn::CPdxIntegerFlags_UpdateFlags, (void*)&UpdateFlagsDetour, (void**)&g_orig_update_flags,
              "CPdxIntegerFlags::UpdateFlags") && ok;
    ok = Hook(base + sdk::fn::CModifierNodeManager_AddInvalid, (void*)&AddInvalidDetour, (void**)&g_orig_add_invalid,
              "CModifierNodeManager::AddInvalid") && ok;
    ok = Hook(base + sdk::fn::CModifierNodeManager_Update, (void*)&ManagerUpdateDetour, (void**)&g_orig_mgr_update,
              "CModifierNodeManager::Update") && ok;
    ok = Hook(base + sdk::fn::CEventTarget_GetScope, (void*)&GetScopeDetour, (void**)&g_orig_get_scope,
              "CEventTarget::GetScope") && ok;
    ok = Hook(base + sdk::fn::CGameState_OnNewGameStarted, (void*)&NewGameStartedDetour, (void**)&g_orig_new_game,
              "CGameState::OnNewGameStarted") && ok;
    ok = Hook(base + sdk::fn::CGameState_OnSavedGameStarted, (void*)&SavedGameStartedDetour,
              (void**)&g_orig_saved_game, "CGameState::OnSavedGameStarted") && ok;
    ok = Hook(base + sdk::fn::CEventScope_Copy, (void*)&ScopeCopyDetour, (void**)&g_orig_scope_copy,
              "CEventScope::Copy") && ok;
    ok = Hook(base + sdk::fn::GetDynamicFlag, (void*)&GetDynamicFlagDetour, (void**)&g_orig_dyn_flag, "GetDynamicFlag") && ok;
    g_installed = ok;
    return ok;
}

void Uninstall() {
    SetFleetGrain1(false);
    if (!g_installed) return;
    g_opinion_on = false;
    g_rule_on = false;
    MH_DisableHook(MH_ALL_HOOKS);
    // A thread can still be inside a detour (or its trampoline) that it entered before the disable.
    // The tick detour lasts a whole tick: wait until it has left and two more ticks completed (or
    // 2 s pass while paused). The opinion/rule detours last micro- to milliseconds; the margin covers
    // them, since they run on the main thread inside a tick or on workers the tick joins.
    const uint64_t done = g_ticks_done.load();
    for (int i = 0; i < 200 && (g_in_tick.load() != 0 || g_ticks_done.load() < done + 2); ++i) Sleep(10);
    for (int i = 0; i < 1000 && g_in_tick.load() != 0; ++i) Sleep(10);
    // a game start lasts as long as the world takes to build: wait for it to be left (up to a minute)
    for (int i = 0; i < 6000 && g_in_start.load() != 0; ++i) Sleep(10);
    Sleep(500);
    MH_Uninitialize();
    {
        std::lock_guard<std::mutex> lock(g_tables_mutex);
        for (void* t : g_tables) free(t);
        g_tables.clear();
    }
    g_installed = false;
    DestroyShared();
}

void Apply(const Settings& s) {
    if (!g_base) return;
    if (s.frame_smoothing >= 0) {
        *(volatile uint8_t*)(g_base + sdk::glob::g_bFrameSmoothing) = s.frame_smoothing ? 1 : 0;
    }
    if (!g_installed) return;
    if (s.profile && !g_profile.load()) {
        g_opinion_prof.Reset();
        g_rule_prof.Reset();
        g_tick_cycles = 0;
        g_profile_day0 = ReadGameDay();
    }
    g_profile = s.profile;
    if (g_opinion_on.exchange(s.opinion_cache) != s.opinion_cache) g_gen.fetch_add(1);
    if (g_rule_mode.exchange(s.rule_cache) != s.rule_cache) g_gen.fetch_add(1);
    g_rule_on = s.rule_cache != 0;
    if (g_fm_on.exchange(s.fleet_manager_cache) != s.fleet_manager_cache) g_gen.fetch_add(1);
    g_reinforce_ms = s.fleet_manager_reinforce_ms;
    if (s.rule_profile && !g_rule_profile.load()) {
        for (int p = 0; p < kRulePaths; ++p) {
            for (int i = 0; i < kRuleIndexSlots; ++i) g_rule_prof_calls[p][i] = 0, g_rule_prof_cycles[p][i] = 0;
        }
    }
    g_rule_profile = s.rule_profile;
    g_flag_mode = s.flag_simd;
    SetFleetGrain1(s.fleet_parallel_grain1);
    if (s.scope_profile && !g_scope_profile.load()) {
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b) {
                for (int c = 0; c < 2; ++c) g_gs_calls[a][b][c] = 0, g_gs_cycles[a][b][c] = 0;
                g_copy_calls[a][b] = 0, g_copy_cycles[a][b] = 0;
            }
        for (int m = 0; m < 2; ++m) g_dyn_calls[m] = 0, g_dyn_cycles[m] = 0, g_dyn_miss[m] = 0;
        g_scope_day0 = ReadGameDay();
    }
    g_scope_profile = s.scope_profile;
    g_flush_max = s.modifier_flush_max > 0 ? (s.modifier_flush_max < kInvCap ? s.modifier_flush_max : kInvCap) : 32;
    if (g_flush_mode.exchange(s.modifier_flush) != s.modifier_flush) {
        g_flush_base = kNoBase;  // the recorded set is stale: the next flush takes the full path
    }
    g_expiry_skip = s.flag_expiry_skip;
    if (SharedStats* shared = g_shared) {
        shared->frame_smoothing = s.frame_smoothing;
        shared->flag_simd = s.flag_simd;
        shared->flag_expiry_skip = s.flag_expiry_skip;
        shared->fleet_parallel_grain1 = g_grain_patched ? 1 : 0;
        shared->opinion_cache = s.opinion_cache;
        shared->rule_cache = s.rule_cache;
        shared->fleet_manager_cache = s.fleet_manager_cache;
        shared->fleet_manager_reinforce_ms = s.fleet_manager_reinforce_ms;
        MemoryBarrier();
        InterlockedIncrement64((volatile LONG64*)&shared->applied_seq);
    }
}

// ---- the settings in effect: the ini, as the multiplayer guard allows -----------------------------
namespace {

std::mutex g_cfg_mutex;
Settings g_user, g_effective;
bool g_user_set = false;
bool g_mp_detected = false;  // the game being played is a multiplayer session (read when a game starts)
bool g_mp_active = false;    // the guard is overriding the settings
bool g_logged_unguarded = false;

bool ReadMultiplayerFlag() {
    if (!g_base) return false;
    __try {
        const uintptr_t idler = *(const uintptr_t*)(g_base + sdk::glob::g_CurrentInGameIdler);
        return idler && *(const uint8_t*)(idler + sdk::rt::CGameIdler_is_multiplayer) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Everything that runs inside or changes the simulation is off; what stays (the fleet manager window
// caches, frame smoothing, the profilers) only changes what this client displays or measures.
Settings ForMultiplayer(const Settings& s) {
    Settings r = s;
    r.opinion_cache = false;
    r.rule_cache = 0;
    r.modifier_flush = 0;
    r.flag_simd = 0;
    r.flag_expiry_skip = false;
    r.fleet_parallel_grain1 = false;
    return r;
}

// g_cfg_mutex held. Applies the settings that are in effect now; true when they changed.
bool Reapply() {
    if (!g_user_set) return false;
    const bool active = g_user.multiplayer_guard == 2 || (g_user.multiplayer_guard == 1 && g_mp_detected);
    const Settings eff = active ? ForMultiplayer(g_user) : g_user;
    if (active != g_mp_active) {
        if (active) {
            Log("%s: settings that touch the simulation are forced off (opinion_cache, rule_cache, modifier_flush, "
                "flag_simd, flag_expiry_skip, fleet_parallel_grain1)",
                g_mp_detected ? "multiplayer game" : "multiplayer_guard=2 (test)");
        } else {
            Log("single-player game: the settings from the ini apply");
        }
    }
    const bool unguarded = g_mp_detected && g_user.multiplayer_guard == 0;
    if (unguarded && !g_logged_unguarded) {
        Log("multiplayer game but multiplayer_guard=0: the settings are NOT overridden; every client must use "
            "identical settings or the game goes out of sync");
    }
    g_logged_unguarded = unguarded;
    if (active == g_mp_active && eff == g_effective) return false;
    g_mp_active = active;
    g_effective = eff;
    Apply(eff);
    Log("settings: frame_smoothing=%d opinion_cache=%d rule_cache=%d fleet_manager_cache=%d "
        "fleet_manager_reinforce_ms=%d flag_simd=%d flag_expiry_skip=%d fleet_parallel_grain1=%d modifier_flush=%d "
        "profile=%d multiplayer_guard_active=%d",
        eff.frame_smoothing, (int)eff.opinion_cache, eff.rule_cache, (int)eff.fleet_manager_cache,
        eff.fleet_manager_reinforce_ms, eff.flag_simd, (int)eff.flag_expiry_skip, (int)eff.fleet_parallel_grain1,
        eff.modifier_flush, (int)eff.profile, (int)active);
    return true;
}

} // namespace

bool SetUserSettings(const Settings& s) {
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    g_user = s;
    g_user_set = true;
    return Reapply();
}

void CheckMultiplayer(const char* why) {
    const bool mp = ReadMultiplayerFlag();
    std::lock_guard<std::mutex> lock(g_cfg_mutex);
    Log("%s: multiplayer flag %d", why, (int)mp);
    g_mp_detected = mp;
    Reapply();
}

void Publish() {
    static int publishes = 0;
    if (g_rule_mode.load() == 2) {
        RuleAdaptiveStep();  // dumps the profile itself after each decision
    } else if (g_rule_profile.load() && ++publishes % 15 == 0) {
        DumpRuleProfile();  // every ~30 s
    }
    if (SharedStats* shared = g_shared) {
        shared->fleet_power_hits = g_fm_stats.hits.load();
        shared->fleet_power_misses = g_fm_stats.misses.load();
        shared->flag_static = g_flag_stats.hits.load();
        shared->flag_dynamic = g_flag_stats.misses.load();
        shared->flag_unreadable = g_flag_stats.bypass.load();
        shared->flag_mismatch = g_flag_mismatch.load();
        shared->expiry_skipped = g_expiry_stats.hits.load();
        shared->expiry_passed = g_expiry_stats.misses.load();
    }
}

std::string FlushStats() {
    const uint64_t fast = g_flush_fast.load(), full = g_flush_full.load();
    char buf[400];
    snprintf(buf, sizeof(buf),
             "modifier_flush=%d fast=%llu full=%llu (why: workers=%llu size=%llu overflow=%llu nobase=%llu) nodes=%llu "
             "sizes[1,2,<=4,<=8,<=16,<=32,>32]=%llu,%llu,%llu,%llu,%llu,%llu,%llu avg_us fast=%.1f full=%.1f mismatches=%llu",
             g_flush_mode.load(), (unsigned long long)fast, (unsigned long long)full,
             (unsigned long long)g_flush_why[0].load(), (unsigned long long)g_flush_why[1].load(),
             (unsigned long long)g_flush_why[2].load(), (unsigned long long)g_flush_why[3].load(),
             (unsigned long long)g_flush_nodes.load(), (unsigned long long)g_flush_hist[0].load(),
             (unsigned long long)g_flush_hist[1].load(), (unsigned long long)g_flush_hist[2].load(),
             (unsigned long long)g_flush_hist[3].load(), (unsigned long long)g_flush_hist[4].load(),
             (unsigned long long)g_flush_hist[5].load(), (unsigned long long)g_flush_hist[6].load(),
             fast ? g_flush_fast_cycles.load() * g_ms_per_cycle * 1000 / fast : 0.0,
             full ? g_flush_full_cycles.load() * g_ms_per_cycle * 1000 / full : 0.0,
             (unsigned long long)g_flush_mismatch.load());
    std::string out = buf;
    if (g_flush_mismatch.load()) {
        out += " mismatch_by_category(cat:bad/checked)=";
        for (int c = 0; c < kMaxCat; ++c) {
            if (g_flush_mismatch_cat[c].load()) {
                out += std::to_string(c) + ":" + std::to_string(g_flush_mismatch_cat[c].load()) + "/" +
                       std::to_string(g_flush_nodes_cat[c].load()) + " ";
            }
        }
    }
    return out;
}

std::string StatsLine() {
    auto one = [](const char* name, const Stats& st, bool on) {
        const uint64_t h = st.hits, m = st.misses, b = st.bypass;
        char buf[160];
        snprintf(buf, sizeof(buf), "%s=%s hits=%llu misses=%llu bypass=%llu hit_rate=%.1f%%", name, on ? "on" : "off",
                 (unsigned long long)h, (unsigned long long)m, (unsigned long long)b, (h + m) ? 100.0 * h / (h + m) : 0.0);
        return std::string(buf);
    };
    const int smoothing = g_base ? *(volatile uint8_t*)(g_base + sdk::glob::g_bFrameSmoothing) : -1;
    std::string prof;
    if (g_profile.load()) {
        // Per game day since profiling started. Thread-local counts publish every 1024 calls, so
        // the last few calls of each thread are missing.
        const double days = (double)(ReadGameDay() - g_profile_day0);
        const double k = days > 0 ? g_ms_per_cycle / days : 0;
        auto part = [&](const char* name, const Prof& p) {
            char buf[200];
            snprintf(buf, sizeof(buf), " | %s main %.2f ms/day %.0f calls/day, other %.2f ms/day %.0f calls/day", name,
                     p.cycles[0] * k, days > 0 ? p.calls[0] / days : 0.0, p.cycles[1] * k,
                     days > 0 ? p.calls[1] / days : 0.0);
            return std::string(buf);
        };
        char head[96];
        snprintf(head, sizeof(head), " || profile days=%.0f tick %.2f ms/day", days, g_tick_cycles.load() * k);
        prof = head + part("opinion", g_opinion_prof) + part("rule", g_rule_prof);
    }
    return "frame_smoothing=" + std::to_string(smoothing) + " | " + one("opinion_cache", g_opinion_stats, g_opinion_on) +
           " | " + one("rule_cache", g_rule_stats, g_rule_on) +
           (g_rule_mode.load() == 2 ? " adaptive: " + std::to_string(g_rule_enabled_count.load()) + " rules cached, est. saving " +
                                          std::to_string((int)(g_rule_saving_ns_per_s.load() / 1e6)) + " ms/s CPU"
                                    : std::string()) + " | " +
           one("fleet_manager_cache", g_fm_stats, g_fm_on) + " | " +
           one("reinforce_throttle(hits=skipped)", g_reinforce_stats, g_reinforce_ms.load() > 0) + " | " +
           one("flag_simd(hits=static misses=dynamic bypass=unreadable)", g_flag_stats, g_flag_mode.load() != 0) +
           " mismatches=" + std::to_string(g_flag_mismatch.load()) + " | " +
           one("flag_expiry_skip(hits=skipped)", g_expiry_stats, g_expiry_skip.load()) +
           " | " + FlushStats() + " | " + ScopeStats() +
           " | gen=" + std::to_string(g_gen.load()) + prof;
}

} // namespace perf
