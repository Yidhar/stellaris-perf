#include "perf_jobs.hpp"
#include "perf_shared.hpp"
#include "perf_tweaks.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace perf::jobs {
namespace {

// 4.5.2: NPlanetJobs::UpdatePopJobs(const CGameDate*, const CPdxSpan<TPdxRef<CColony>>*) and the two pairs of calls
// to it, as the `call` instructions (checked at install): NColonyUpdate::MonthlyUpdate (0xE3BFA0) and the cheat-mode
// daily variant (0xE3BF20). The second call of each pair is the one skipped.
constexpr uintptr_t kRvaUpdatePopJobs = 0xB25DF0;
const uint8_t kHead[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C };
constexpr uintptr_t kFirstCalls[] = { 0xE3C189, 0xE3BF5E };
constexpr uintptr_t kSecondCalls[] = { 0xE3C1AE, 0xE3BF86 };
constexpr int kPairs = 2;

using Fn = uint64_t (*)(void*, void*, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
Fn g_orig = nullptr;
uintptr_t g_base = 0;
bool g_installed = false;
std::atomic<bool> g_on{ false };
uintptr_t g_first[kPairs], g_second[kPairs];  // return addresses
std::atomic<uint64_t> g_first_calls{ 0 }, g_second_calls{ 0 }, g_skipped{ 0 };
// the passes timed (QueryPerformanceCounter): total and longest, in microseconds, per kind of call
std::atomic<uint64_t> g_first_us{ 0 }, g_first_max_us{ 0 }, g_second_us{ 0 };
double g_qpc_us = 0;

// pop_faction_batch: CPopFaction::UpdateSupportAndApproval (4.5.2 0x699D60; Linux 1088784), which the engine runs for
// every faction a pop group belongs to each time pops move between groups (the collapse and the promotions of a
// pass): CalcSupport, then CalcApproval (the faction type's demands, scripted triggers), then, when the approval
// changed, the member groups' modifier nodes invalidated and the owner flagged. Within one pass nothing reads a
// faction's support or approval between the moves, so with the batch on the calls inside a pass are recorded and run
// once per faction when the pass ends (in order of first appearance): the same final values from the same final
// state; a faction whose values changed and changed back inside the pass is not invalidated or flagged then.
// Modes: 0 = the engine's calls as they come, counted and timed inside the passes; 1 = batched over each pass;
// 2 = batched over the whole NColonyUpdate::MonthlyUpdate (0xE3BFA0: growth, migration, decline, assembly, the two
// passes), which also covers the moves of the monthly colony pass; 3 = verify: the engine's calls as they come, and
// at the end of each pass every faction touched is recomputed once more from the final state and its stored support
// and approval compared with what the engine's last call left (a mismatch is logged and counted); 4 = the same
// check over the whole monthly update (mode 2's window).
constexpr uintptr_t kRvaUpdateSupportAndApproval = 0x699D60;
const uint8_t kHeadFaction[8] = { 0x40, 0x55, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B };
constexpr uintptr_t kRvaColonyMonthlyUpdate = 0xE3BFA0;  // NColonyUpdate::MonthlyUpdate (the caller of the two passes)
const uint8_t kHeadColonyMonthly[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C };
constexpr size_t kFactionSupport = 0x40;  // 16 bytes of support, then the approval at +0x50 (8 bytes)
constexpr size_t kFactionValues = 0x18;
using FactionFn = void (*)(void*);
FactionFn g_orig_faction = nullptr;
Fn g_orig_colony_monthly = nullptr;
bool g_faction_installed = false;
std::atomic<int> g_faction_mode{ 0 };
thread_local int t_depth = 0;      // windows open on this thread (the main thread): a pass, or the monthly update
thread_local int t_window_mode = 0;  // the mode the outermost window was opened with
constexpr int kMaxPending = 4096;
void* g_pending[kMaxPending];  // the factions recorded in the window in progress (main thread only)
int g_pending_n = 0;
bool g_pending_overflow = false;
std::atomic<uint64_t> g_fac_calls{ 0 }, g_fac_distinct{ 0 }, g_fac_deferred{ 0 }, g_fac_us{ 0 }, g_fac_windows{ 0 };
std::atomic<uint64_t> g_fac_verified{ 0 }, g_fac_mismatch{ 0 };

uint64_t Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}

void FactionDetour(void* faction) {
    if (t_depth == 0) {
        g_orig_faction(faction);
        return;
    }
    g_fac_calls.fetch_add(1, std::memory_order_relaxed);
    bool seen = false;
    for (int i = 0; i < g_pending_n; ++i) {
        if (g_pending[i] == faction) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        if (g_pending_n < kMaxPending) g_pending[g_pending_n++] = faction;
        else g_pending_overflow = true;
    }
    const bool batch = t_window_mode == 1 || t_window_mode == 2;
    if (batch && !g_pending_overflow) {
        g_fac_deferred.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t t0 = Now();
    g_orig_faction(faction);
    g_fac_us.fetch_add((uint64_t)((double)(Now() - t0) * g_qpc_us), std::memory_order_relaxed);
}

void BeginWindow(int mode) {
    if (t_depth++ == 0) {
        t_window_mode = mode;
        g_pending_n = 0;
        g_pending_overflow = false;
    }
}

// The end of the outermost window: the recorded factions updated once each when batching; in verify mode recomputed
// once more and compared with what the engine's own calls left; the counters settled.
void EndWindow() {
    if (--t_depth != 0) return;
    const uint64_t t0 = Now();
    if (t_window_mode == 1 || t_window_mode == 2) {
        for (int i = 0; i < g_pending_n; ++i) g_orig_faction(g_pending[i]);
    } else if (t_window_mode == 3 || t_window_mode == 4) {
        for (int i = 0; i < g_pending_n; ++i) {
            uint8_t before[kFactionValues];
            memcpy(before, (const char*)g_pending[i] + kFactionSupport, kFactionValues);
            g_orig_faction(g_pending[i]);
            g_fac_verified.fetch_add(1, std::memory_order_relaxed);
            if (memcmp(before, (const char*)g_pending[i] + kFactionSupport, kFactionValues) != 0) {
                g_fac_mismatch.fetch_add(1, std::memory_order_relaxed);
                Log("jobs: pop_faction_batch verify: faction %p recomputed from the final state differs from the engine's last value",
                    g_pending[i]);
            }
        }
    }
    g_fac_us.fetch_add((uint64_t)((double)(Now() - t0) * g_qpc_us), std::memory_order_relaxed);
    g_fac_distinct.fetch_add((uint64_t)g_pending_n, std::memory_order_relaxed);
    g_fac_windows.fetch_add(1, std::memory_order_relaxed);
    g_pending_n = 0;
    g_pending_overflow = false;
}

uint64_t ColonyMonthlyDetour(void* a, void* b, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    const int mode = g_faction_mode.load(std::memory_order_relaxed);
    const bool window = mode == 2 || mode == 4;
    if (window) BeginWindow(mode);
    const uint64_t r = g_orig_colony_monthly(a, b, c, d, e, f, g, h);
    if (window) EndWindow();
    return r;
}

uint64_t Detour(void* a, void* b, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    int kind = 0;  // 1 = the first call of a pair, 2 = the second
    for (int i = 0; i < kPairs; ++i) {
        if (ret == g_second[i]) {
            kind = 2;
            g_second_calls.fetch_add(1, std::memory_order_relaxed);
            if (g_on.load(std::memory_order_relaxed)) {
                g_skipped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            break;
        }
        if (ret == g_first[i]) {
            kind = 1;
            g_first_calls.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    if (kind == 0) return g_orig(a, b, c, d, e, f, g, h);
    SharedStats* shared = kind == 1 ? Shared() : nullptr;  // the first pass's window, for a sampling profiler outside the game
    if (shared) {
        shared->jobs_tid = GetCurrentThreadId();
        InterlockedExchange((volatile LONG*)&shared->jobs_active, 1);
    }
    // a window over the pass (inside the monthly window of mode 2 it only nests: that one flushes at its end)
    if (g_faction_installed) BeginWindow(g_faction_mode.load(std::memory_order_relaxed));
    const uint64_t t0 = Now();
    const uint64_t r = g_orig(a, b, c, d, e, f, g, h);
    if (g_faction_installed) EndWindow();
    const uint64_t us = (uint64_t)((double)(Now() - t0) * g_qpc_us);
    if (shared) InterlockedExchange((volatile LONG*)&shared->jobs_active, 0);
    if (kind == 1) {
        g_first_us.fetch_add(us, std::memory_order_relaxed);
        uint64_t m = g_first_max_us.load(std::memory_order_relaxed);
        while (us > m && !g_first_max_us.compare_exchange_weak(m, us)) {
        }
    } else {
        g_second_us.fetch_add(us, std::memory_order_relaxed);
    }
    return r;
}

bool CallsTo(uintptr_t at, uintptr_t target) {
    const uint8_t* p = (const uint8_t*)(g_base + at);
    return p[0] == 0xE8 && at + 5 + (uintptr_t)(int64_t)*(const int32_t*)(p + 1) == target;
}

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_us = 1e6 / (double)freq.QuadPart;
    if (memcmp((const void*)(base + kRvaUpdatePopJobs), kHead, 8) != 0) {
        Log("jobs: NPlanetJobs::UpdatePopJobs at RVA 0x%llX does not start as expected; pop_jobs_single_pass unavailable",
            (unsigned long long)kRvaUpdatePopJobs);
        return false;
    }
    for (int i = 0; i < kPairs; ++i) {
        if (!CallsTo(kFirstCalls[i], kRvaUpdatePopJobs) || !CallsTo(kSecondCalls[i], kRvaUpdatePopJobs)) {
            Log("jobs: no pair of calls to UpdatePopJobs at RVA 0x%llX / 0x%llX; pop_jobs_single_pass unavailable",
                (unsigned long long)kFirstCalls[i], (unsigned long long)kSecondCalls[i]);
            return false;
        }
        g_first[i] = base + kFirstCalls[i] + 5;
        g_second[i] = base + kSecondCalls[i] + 5;
    }
    const LPVOID target = (LPVOID)(base + kRvaUpdatePopJobs);
    if (MH_CreateHook(target, (LPVOID)&Detour, (LPVOID*)&g_orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("jobs: could not hook NPlanetJobs::UpdatePopJobs; pop_jobs_single_pass unavailable");
        return false;
    }
    g_installed = true;
    Log("jobs: hooked NPlanetJobs::UpdatePopJobs (pop_jobs_single_pass)");
    if (memcmp((const void*)(base + kRvaUpdateSupportAndApproval), kHeadFaction, 8) != 0) {
        Log("jobs: CPopFaction::UpdateSupportAndApproval at RVA 0x%llX does not start as expected; pop_faction_batch unavailable",
            (unsigned long long)kRvaUpdateSupportAndApproval);
        return true;
    }
    if (memcmp((const void*)(base + kRvaColonyMonthlyUpdate), kHeadColonyMonthly, 8) != 0) {
        Log("jobs: NColonyUpdate::MonthlyUpdate at RVA 0x%llX does not start as expected; pop_faction_batch unavailable",
            (unsigned long long)kRvaColonyMonthlyUpdate);
        return true;
    }
    const LPVOID faction = (LPVOID)(base + kRvaUpdateSupportAndApproval), monthly = (LPVOID)(base + kRvaColonyMonthlyUpdate);
    if (MH_CreateHook(faction, (LPVOID)&FactionDetour, (LPVOID*)&g_orig_faction) != MH_OK ||
        MH_CreateHook(monthly, (LPVOID)&ColonyMonthlyDetour, (LPVOID*)&g_orig_colony_monthly) != MH_OK ||
        MH_EnableHook(faction) != MH_OK || MH_EnableHook(monthly) != MH_OK) {
        Log("jobs: could not hook CPopFaction::UpdateSupportAndApproval / NColonyUpdate::MonthlyUpdate; pop_faction_batch unavailable");
        return true;
    }
    g_faction_installed = true;
    Log("jobs: hooked CPopFaction::UpdateSupportAndApproval and NColonyUpdate::MonthlyUpdate (pop_faction_batch)");
    return true;
}

void SetEnabled(bool on) {
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("pop_jobs_single_pass=%d", on ? 1 : 0);
}

void SetFactionMode(int mode) {
    if (!g_faction_installed) return;
    if (g_faction_mode.exchange(mode) != mode) Log("pop_faction_batch=%d", mode);
}

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "pop_jobs_single_pass=unavailable";
    char buf[240];
    const uint64_t first = g_first_calls.load(), second = g_second_calls.load(), skipped = g_skipped.load();
    const uint64_t ran2 = second - skipped;
    snprintf(buf, sizeof buf, "pop_jobs_single_pass=%s(first=%llu second=%llu skipped=%llu first_ms=%.1f max=%.1f second_ms=%.1f)",
             g_on.load() ? "on" : "off", (unsigned long long)first, (unsigned long long)second, (unsigned long long)skipped,
             first ? (double)g_first_us.load() / 1000.0 / (double)first : 0.0, (double)g_first_max_us.load() / 1000.0,
             ran2 ? (double)g_second_us.load() / 1000.0 / (double)ran2 : 0.0);
    std::string out(buf);
    if (!g_faction_installed) return out + " | pop_faction_batch=unavailable";
    const uint64_t windows = g_fac_windows.load(), calls = g_fac_calls.load(), distinct = g_fac_distinct.load();
    snprintf(buf, sizeof buf,
             " | pop_faction_batch=%d(windows=%llu calls/window=%.0f factions/window=%.0f deferred=%llu ms/window=%.2f verified=%llu mismatches=%llu)",
             g_faction_mode.load(), (unsigned long long)windows, windows ? (double)calls / (double)windows : 0.0,
             windows ? (double)distinct / (double)windows : 0.0, (unsigned long long)g_fac_deferred.load(),
             windows ? (double)g_fac_us.load() / 1000.0 / (double)windows : 0.0, (unsigned long long)g_fac_verified.load(),
             (unsigned long long)g_fac_mismatch.load());
    return out + buf;
}

}  // namespace perf::jobs
