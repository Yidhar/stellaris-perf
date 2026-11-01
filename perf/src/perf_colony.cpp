#include "perf_colony.hpp"
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

inline bool IsSite(const uintptr_t* sites, int n, uintptr_t ret) {
    for (int i = 0; i < n; ++i) {
        if (sites[i] == ret) return true;
    }
    return false;
}

uint64_t UpdateDetour(void* node) {
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

bool Sites(const uintptr_t* calls, int n, uintptr_t target, uintptr_t* out, const char* what) {
    for (int i = 0; i < n; ++i) {
        if (!CallsTo(calls[i], target)) {
            Log("colony: no call to %s at RVA 0x%llX; colony_monthly_batch unavailable", what, (unsigned long long)calls[i]);
            return false;
        }
        out[i] = g_base + calls[i] + 5;
    }
    return true;
}

}  // namespace

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
    if (!Sites(kColonyUpdateCalls, kColonySites, sdk::fn::CModifierNodeBase_Update, g_colony_sites, "CModifierNodeBase::Update") ||
        !Sites(kGroupUpdateCalls, kGroupSites, sdk::fn::CModifierNodeBase_Update, g_group_sites, "CModifierNodeBase::Update") ||
        !Sites(kCachedCalls, kCachedSites, kRvaCachedValues, g_cached_sites, "CColony::MicroUpdateCachedValues")) {
        return false;
    }
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
    return std::string(buf);
}

}  // namespace perf::colony
