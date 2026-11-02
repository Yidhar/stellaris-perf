#include "perf_colony.hpp"
#include "perf_shared.hpp"
#include "perf_tweaks.hpp"
#include "MinHook.h"
#include "stellaris_sdk.hpp"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace perf::colony {
namespace {

// 4.5.2 entry points not in the SDK (each checked against its first bytes) and the offsets the engine's own code uses
constexpr uintptr_t kRvaMonthlySerial = 0xDC43E0;   // CColony::MonthlyUpdateSerial(this, SDemographySafePrivate*, int*)
constexpr uintptr_t kRvaNodeClear = 0x30E9F0;       // CModifierNodeBase::Clear(node)
constexpr uintptr_t kRvaCachedValues = 0xDD6950;    // CColony::MicroUpdateCachedValues(this, CModifier*) -> bool changed
constexpr uintptr_t kRvaInvalidate = 0xDACEF0;      // CColony::InvalidateModifier(this)
constexpr ptrdiff_t kColonyNode = 0x120;            // the colony's modifier node
constexpr ptrdiff_t kColonyModifier = 0x170;        // the CModifier the cached values are computed from
constexpr ptrdiff_t kColonyFlags = 0x1088;          // bit 2 set: the node is cleared instead of updated

const uint8_t kHeadMonthly[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };
const uint8_t kHeadUpdate[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };
const uint8_t kHeadCached[8] = { 0x40, 0x53, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x28 };
const uint8_t kHeadClear[8] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B };
const uint8_t kHeadInvalidate[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };

// The refreshes the engine forces after every change, as the `call` instructions that make them (the detours see the
// return address, 5 bytes on):
//   CColony::OnAddPops tail (0xDAFAB8)   0xDAFAD3 Update(colony node)   0xDAFAE2 MicroUpdateCachedValues(colony)
//   CColony::OnRemovePops   (0xDAFB10)   0xDAFB55 Update(colony node)   0xDAFB64 MicroUpdateCachedValues(colony)
//   AddSize to a new pop group     (0xDB0BB0)   0xDB0C34 Update(the new group's node)
//   MoveSizeFrom to a new pop group (0xDB0C60)  0xDB0CF0 / 0xDB0CF9 Update(source / new group's node)
// In the first two the Update is skipped only for the colony whose pass is running: any other colony touched from
// inside the pass (an event effect) gets its cached values recomputed at once, which needs its node fresh.
constexpr uintptr_t kColonyUpdateCalls[] = { 0xDAFAD3, 0xDAFB55 };
constexpr uintptr_t kGroupUpdateCalls[] = { 0xDB0C34, 0xDB0CF0, 0xDB0CF9 };
constexpr uintptr_t kCachedCalls[] = { 0xDAFAE2, 0xDAFB64 };
constexpr int kColonySites = 2, kGroupSites = 3, kCachedSites = 2;
constexpr int kMaxPending = 64;  // skipped pop-group refreshes per pass; beyond that they run as the engine does

// pop_jobs_node_batch: the two refreshes the pop-job pass forces for every pop group it creates. The promotion loop
// of NPlanetJobs::AssignPopsToJobs (0xB35F30, the tail of every UpdatePopJobs pass) has
// CColony::AccessOrAddPopGroupMoveSizeFrom inlined: a move into an existing group is CPopGroup::MoveSizeFrom alone, a
// move into a new group is followed by CModifierNodeBase::Update of the source group's node and of the new group's
// node (the calls at 0xB364C1 / 0xB364CA; the out-of-line copy used by growth and migration is the pair above). Each
// of them rebuilds the colony's and the planet's node first, dirty again at every one of them (a group's rebuild
// invalidates them: replaying the refreshes at the end of the pass costs the same, mode 1), so on a late save the
// two calls are 6 ms of a 50 ms pass (half of it the colony and planet rebuilds, scripted triggers), while nothing in
// the rest of the pass reads a pop group's node (the promotions decide by precomputed category flags) and the nodes
// are rebuilt on demand by their next reader anyway, like any node the engine invalidates. Watched over the month
// after the pass (mode 3), half of the nodes refreshed are never read again before the next pass (the source groups
// emptied by a whole-group promotion, flagged dead, and groups nobody consults), a tenth are rebuilt by the modifier
// manager's flush, the rest on demand by a reader inside the monthly update. A group emptied by a move is only
// flagged (CColony::RemovePopGroup); its node stays until the dead objects are swept, so a replay at the end of the
// pass touches live memory, as the engine's own call does.
//   2: the two refreshes are skipped; the nodes stay dirty until their next reader rebuilds them (the default).
//   1: skipped and the nodes they named rebuilt once each when the pass ends (first-appearance order): the same
//      values from the same final state of the pass, but no gain (see above).
//   0: the engine's refreshes, counted and timed; at the end of the pass each node named is checked for whether it was
//      invalidated again after its refresh (dirty_end) or not (clean_end).
//   3: measurement: 2, and the nodes left dirty watched until the next pass (reread_in / reread_out = rebuilt on demand
//      inside / outside the monthly update, timed; flushed = clean at the next pass without a rebuild seen, the
//      manager's flush; unread = still dirty at the next pass).
constexpr uintptr_t kJobsUpdateCalls[] = { 0xB364C1, 0xB364CA };
constexpr int kJobsSites = 2;
constexpr int kMaxJobsPending = 4096;  // nodes per pass; beyond that the engine's refreshes run as they come
constexpr ptrdiff_t kNodeDirty = 0x108;  // CModifierNodeBase: the byte Update tests and clears

using NodeFn = uint64_t (*)(void*);
using CachedFn = uint64_t (*)(void*, void*);
using MonthlyFn = uint64_t (*)(void*, void*, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
NodeFn g_orig_update = nullptr;
NodeFn g_clear = nullptr;
CachedFn g_orig_cached = nullptr;
NodeFn g_invalidate = nullptr;
MonthlyFn g_orig_monthly = nullptr;
uintptr_t g_base = 0;
bool g_installed = false;
std::atomic<int> g_mode{ 0 };
uintptr_t g_colony_sites[kColonySites], g_group_sites[kGroupSites], g_cached_sites[kCachedSites];  // return addresses

// The pass in progress. It runs on the main thread only (the parallel demography pass before it computes the amounts
// it applies), so the detours on other threads see no pass and change nothing.
thread_local void* t_colony = nullptr;
thread_local int t_mode = 0;                  // the mode the pass started with
thread_local bool t_colony_dirty = false;     // a refresh of the colony's node or cached values was skipped
thread_local int t_pending_count = 0;
thread_local void* t_pending[kMaxPending];    // pop-group nodes whose refresh was skipped

std::atomic<uint64_t> g_passes{ 0 }, g_batched{ 0 }, g_skipped{ 0 }, g_flushes_held{ 0 }, g_frozen_reads{ 0 };

// The pop-job pass in progress (main thread only; perf_jobs opens it around every UpdatePopJobs pass)
uintptr_t g_jobs_sites[kJobsSites];
bool g_jobs_installed = false;
std::atomic<int> g_jobs_mode{ 0 };
thread_local int t_jobs_depth = 0;
thread_local int t_jobs_mode = 0;             // the mode the pass started with
void* g_jobs_pending[kMaxJobsPending];       // the nodes named by the skipped refreshes, first appearance first
int g_jobs_pending_n = 0;
bool g_jobs_overflow = false;
double g_qpc_us = 0;
std::atomic<uint64_t> g_jobs_passes{ 0 }, g_jobs_calls{ 0 }, g_jobs_deferred{ 0 }, g_jobs_replayed{ 0 }, g_jobs_us{ 0 };
std::atomic<uint64_t> g_jobs_dirty_end{ 0 }, g_jobs_clean_end{ 0 };
// mode 3 (measurement): mode 2, and the nodes left dirty are watched until the next pass: a rebuild on demand by a
// reader (an Update finding one dirty, inside the monthly update or outside it), a rebuild by the modifier manager's
// flush (clean at the next pass without an Update seen), or still dirty at the next pass (never read)
void* g_jobs_watch[kMaxJobsPending];
std::atomic<int> g_jobs_watch_n{ 0 };
std::atomic<uint64_t> g_jobs_reread_in{ 0 }, g_jobs_reread_out{ 0 }, g_jobs_flushed{ 0 }, g_jobs_unread{ 0 }, g_jobs_reread_us{ 0 };

uint64_t Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}

inline bool IsSite(const uintptr_t* sites, int n, uintptr_t ret) {
    for (int i = 0; i < n; ++i) {
        if (sites[i] == ret) return true;
    }
    return false;
}

// One of the pass's forced refreshes: recorded (once per node), skipped when batching, else run and timed.
uint64_t JobsSiteUpdate(void* node) {
    g_jobs_calls.fetch_add(1, std::memory_order_relaxed);
    bool seen = false;
    for (int i = 0; i < g_jobs_pending_n; ++i) {
        if (g_jobs_pending[i] == node) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        if (g_jobs_pending_n < kMaxJobsPending) g_jobs_pending[g_jobs_pending_n++] = node;
        else g_jobs_overflow = true;
    }
    if (t_jobs_mode != 0 && !g_jobs_overflow) {
        g_jobs_deferred.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t t0 = Now();
    const uint64_t r = g_orig_update(node);
    g_jobs_us.fetch_add((uint64_t)((double)(Now() - t0) * g_qpc_us), std::memory_order_relaxed);
    return r;
}

bool NodeDirty(const void* node) {
    __try {
        return *((const uint8_t*)node + kNodeDirty) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// mode 3: a watched node's first rebuild on demand, counted by where it happens and timed; false when the node is not
// one of them or is not dirty (the caller runs the engine's Update as usual)
bool WatchRead(void* node, uint64_t* result) {
    const int n = g_jobs_watch_n.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        if (g_jobs_watch[i] != node) continue;
        if (!NodeDirty(node)) return false;
        g_jobs_watch[i] = nullptr;
        SharedStats* shared = Shared();
        (shared && shared->monthly_active ? g_jobs_reread_in : g_jobs_reread_out).fetch_add(1, std::memory_order_relaxed);
        const uint64_t t0 = Now();
        *result = g_orig_update(node);
        g_jobs_reread_us.fetch_add((uint64_t)((double)(Now() - t0) * g_qpc_us), std::memory_order_relaxed);
        return true;
    }
    return false;
}

uint64_t UpdateDetour(void* node) {
    if (t_jobs_depth && IsSite(g_jobs_sites, kJobsSites, (uintptr_t)_ReturnAddress())) return JobsSiteUpdate(node);
    if (g_jobs_watch_n.load(std::memory_order_relaxed) > 0) {
        uint64_t r;
        if (WatchRead(node, &r)) return r;
    }
    if (t_colony) {
        const uintptr_t ret = (uintptr_t)_ReturnAddress();
        if (node == (uint8_t*)t_colony + kColonyNode) {
            if (t_mode == 2) {  // frozen: every reader gets the node as it was when the pass began
                t_colony_dirty = true;
                g_frozen_reads.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            if (IsSite(g_colony_sites, kColonySites, ret)) {
                t_colony_dirty = true;
                g_skipped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        } else if (t_pending_count < kMaxPending && IsSite(g_group_sites, kGroupSites, ret)) {
            t_pending[t_pending_count++] = node;
            g_skipped.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return g_orig_update(node);
}

uint64_t CachedDetour(void* colony, void* modifier) {
    if (t_colony == colony && IsSite(g_cached_sites, kCachedSites, (uintptr_t)_ReturnAddress())) {
        t_colony_dirty = true;
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return 0;  // "nothing changed": the invalidation that would follow is done at the end of the pass
    }
    return g_orig_cached(colony, modifier);
}

// What CColony::OnAddPops / OnRemovePops do after a change: the node up to date, the cached values recomputed from it,
// the node invalidated again when they changed.
void Refresh(void* colony) {
    uint8_t* c = (uint8_t*)colony;
    if ((*(const uint32_t*)(c + kColonyFlags) >> 2) & 1) g_clear(c + kColonyNode);
    else g_orig_update(c + kColonyNode);
    if (g_orig_cached(colony, c + kColonyModifier) & 0xFF) g_invalidate(colony);
}

uint64_t MonthlyDetour(void* colony, void* a, void* b, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0 || t_colony) return g_orig_monthly(colony, a, b, d, e, f, g, h);
    t_colony = colony;
    t_mode = mode;
    t_colony_dirty = false;
    t_pending_count = 0;
    g_passes.fetch_add(1, std::memory_order_relaxed);
    const uint64_t r = g_orig_monthly(colony, a, b, d, e, f, g, h);
    t_colony = nullptr;  // the refreshes below must run for real
    if (t_colony_dirty || t_pending_count) {
        // the one refresh that replaces the skipped ones, in the engine's own order
        for (int i = 0; i < t_pending_count; ++i) g_orig_update(t_pending[i]);
        if (t_colony_dirty) Refresh(colony);
        // frozen: nodes rebuilt from the frozen node during the pass (dirty parents are rebuilt first) hold values
        // computed from it; the invalidation marks them again so the next read rebuilds them from the fresh node
        if (mode == 2) g_invalidate(colony);
        g_batched.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

bool CallsTo(uintptr_t at, uintptr_t target) {
    const uint8_t* p = (const uint8_t*)(g_base + at);
    return p[0] == 0xE8 && at + 5 + (uintptr_t)(int64_t)*(const int32_t*)(p + 1) == target;
}

bool Sites(const uintptr_t* calls, int n, uintptr_t target, uintptr_t* out, const char* what, const char* feature) {
    for (int i = 0; i < n; ++i) {
        if (!CallsTo(calls[i], target)) {
            Log("colony: no call to %s at RVA 0x%llX; %s unavailable", what, (unsigned long long)calls[i], feature);
            return false;
        }
        out[i] = g_base + calls[i] + 5;
    }
    return true;
}

}  // namespace

void BeginJobsPass() {
    if (!g_jobs_installed) return;
    if (t_jobs_depth++ == 0) {
        t_jobs_mode = g_jobs_mode.load(std::memory_order_relaxed);
        g_jobs_pending_n = 0;
        g_jobs_overflow = false;
        // mode 3: what became of the nodes the previous pass left dirty and nobody rebuilt on demand
        const int n = g_jobs_watch_n.exchange(0, std::memory_order_relaxed);
        uint64_t unread = 0, flushed = 0;
        for (int i = 0; i < n; ++i) {
            if (!g_jobs_watch[i]) continue;
            (NodeDirty(g_jobs_watch[i]) ? unread : flushed) += 1;
        }
        g_jobs_unread.fetch_add(unread, std::memory_order_relaxed);
        g_jobs_flushed.fetch_add(flushed, std::memory_order_relaxed);
    }
}

// The end of the pass: the skipped refreshes replayed once per node (mode 1), dropped (mode 2), or, with the engine's
// refreshes run as they came (mode 0), each node checked for whether it is dirty again.
void EndJobsPass() {
    if (!g_jobs_installed || t_jobs_depth == 0 || --t_jobs_depth != 0) return;
    const int mode = t_jobs_mode;
    t_jobs_mode = 0;
    const uint64_t t0 = Now();
    if (mode == 1) {
        for (int i = 0; i < g_jobs_pending_n; ++i) g_orig_update(g_jobs_pending[i]);
        g_jobs_replayed.fetch_add((uint64_t)g_jobs_pending_n, std::memory_order_relaxed);
    } else if (mode == 0) {
        uint64_t dirty = 0;
        for (int i = 0; i < g_jobs_pending_n; ++i) dirty += NodeDirty(g_jobs_pending[i]) ? 1 : 0;
        g_jobs_dirty_end.fetch_add(dirty, std::memory_order_relaxed);
        g_jobs_clean_end.fetch_add((uint64_t)g_jobs_pending_n - dirty, std::memory_order_relaxed);
    } else if (mode == 3) {
        memcpy(g_jobs_watch, g_jobs_pending, sizeof(void*) * (size_t)g_jobs_pending_n);
        g_jobs_watch_n.store(g_jobs_pending_n, std::memory_order_release);
    }
    g_jobs_us.fetch_add((uint64_t)((double)(Now() - t0) * g_qpc_us), std::memory_order_relaxed);
    g_jobs_passes.fetch_add(1, std::memory_order_relaxed);
    g_jobs_pending_n = 0;
    g_jobs_overflow = false;
}

void SetJobsMode(int mode) {
    if (!g_jobs_installed) return;
    if (mode < 0 || mode > 3) mode = 0;
    if (g_jobs_mode.exchange(mode) != mode) Log("pop_jobs_node_batch=%d", mode);
}

bool Install(uintptr_t base) {
    g_base = base;
    struct Check { uintptr_t rva; const uint8_t* head; const char* name; };
    const Check checks[] = {
        { kRvaMonthlySerial, kHeadMonthly, "CColony::MonthlyUpdateSerial" },
        { sdk::fn::CModifierNodeBase_Update, kHeadUpdate, "CModifierNodeBase::Update" },
        { kRvaCachedValues, kHeadCached, "CColony::MicroUpdateCachedValues" },
        { kRvaNodeClear, kHeadClear, "CModifierNodeBase::Clear" },
        { kRvaInvalidate, kHeadInvalidate, "CColony::InvalidateModifier" },
    };
    for (const Check& c : checks) {
        if (memcmp((const void*)(base + c.rva), c.head, 8) != 0) {
            Log("colony: %s at RVA 0x%llX does not start as expected; colony_monthly_batch unavailable", c.name, (unsigned long long)c.rva);
            return false;
        }
    }
    if (!Sites(kColonyUpdateCalls, kColonySites, sdk::fn::CModifierNodeBase_Update, g_colony_sites, "CModifierNodeBase::Update", "colony_monthly_batch") ||
        !Sites(kGroupUpdateCalls, kGroupSites, sdk::fn::CModifierNodeBase_Update, g_group_sites, "CModifierNodeBase::Update", "colony_monthly_batch") ||
        !Sites(kCachedCalls, kCachedSites, kRvaCachedValues, g_cached_sites, "CColony::MicroUpdateCachedValues", "colony_monthly_batch")) {
        return false;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_us = 1e6 / (double)freq.QuadPart;
    g_clear = (NodeFn)(base + kRvaNodeClear);
    g_invalidate = (NodeFn)(base + kRvaInvalidate);
    const LPVOID update = (LPVOID)(base + sdk::fn::CModifierNodeBase_Update), cached = (LPVOID)(base + kRvaCachedValues),
                 monthly = (LPVOID)(base + kRvaMonthlySerial);
    if (MH_CreateHook(update, (LPVOID)&UpdateDetour, (LPVOID*)&g_orig_update) != MH_OK ||
        MH_CreateHook(cached, (LPVOID)&CachedDetour, (LPVOID*)&g_orig_cached) != MH_OK ||
        MH_CreateHook(monthly, (LPVOID)&MonthlyDetour, (LPVOID*)&g_orig_monthly) != MH_OK ||
        MH_EnableHook(update) != MH_OK || MH_EnableHook(cached) != MH_OK || MH_EnableHook(monthly) != MH_OK) {
        Log("colony: could not hook; colony_monthly_batch unavailable");
        return false;
    }
    g_installed = true;
    Log("colony: hooked CColony::MonthlyUpdateSerial, CModifierNodeBase::Update and CColony::MicroUpdateCachedValues (colony_monthly_batch)");
    // the pop-job pass's refreshes share the Update hook
    if (Sites(kJobsUpdateCalls, kJobsSites, sdk::fn::CModifierNodeBase_Update, g_jobs_sites, "CModifierNodeBase::Update", "pop_jobs_node_batch")) {
        g_jobs_installed = true;
        Log("colony: the pop-job pass's two refreshes per new group found (pop_jobs_node_batch)");
    }
    return true;
}

void SetMode(int mode) {
    if (!g_installed) return;
    if (mode < 0 || mode > 2) mode = 0;
    if (g_mode.exchange(mode) != mode) Log("colony_monthly_batch=%d", mode);
}

bool HoldFlush() {
    if (!t_colony) return false;
    g_flushes_held.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "colony_monthly_batch=unavailable";
    char buf[200];
    snprintf(buf, sizeof buf, "colony_monthly_batch=%d(passes=%llu batched=%llu skipped=%llu flushes_held=%llu frozen_reads=%llu)",
             g_mode.load(), (unsigned long long)g_passes.load(), (unsigned long long)g_batched.load(),
             (unsigned long long)g_skipped.load(), (unsigned long long)g_flushes_held.load(), (unsigned long long)g_frozen_reads.load());
    std::string out(buf);
    if (!g_jobs_installed) return out + " | pop_jobs_node_batch=unavailable";
    const uint64_t passes = g_jobs_passes.load();
    snprintf(buf, sizeof buf,
             " | pop_jobs_node_batch=%d(passes=%llu calls/pass=%.1f deferred=%llu replayed=%llu ms/pass=%.2f dirty_end=%llu clean_end=%llu"
             " reread_in=%llu reread_out=%llu reread_ms/pass=%.2f flushed=%llu unread=%llu)",
             g_jobs_mode.load(), (unsigned long long)passes, passes ? (double)g_jobs_calls.load() / (double)passes : 0.0,
             (unsigned long long)g_jobs_deferred.load(), (unsigned long long)g_jobs_replayed.load(),
             passes ? (double)g_jobs_us.load() / 1000.0 / (double)passes : 0.0, (unsigned long long)g_jobs_dirty_end.load(),
             (unsigned long long)g_jobs_clean_end.load(), (unsigned long long)g_jobs_reread_in.load(),
             (unsigned long long)g_jobs_reread_out.load(), passes ? (double)g_jobs_reread_us.load() / 1000.0 / (double)passes : 0.0,
             (unsigned long long)g_jobs_flushed.load(), (unsigned long long)g_jobs_unread.load());
    return out + buf;
}

}  // namespace perf::colony
