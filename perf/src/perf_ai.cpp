#include "perf_ai.hpp"
#include "perf_tweaks.hpp"
#include "perf_shared.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace perf::ai {
namespace {

// From the SDK (tools/sdk_dumper/functions.py in the Stellaris MCP repository)
constexpr uintptr_t kRvaUpdateAIParallel = sdk::fn::UpdateAIParallel;  // UpdateAIParallel(CCountryAI* const* countries, int n)
// CPdxParallelForDescriptor::CPdxParallelForDescriptor(this, begin, end, job type, max tasks, pair<scheduler,
// priority>*): writes the task count (threads of the priority + 1; the max-task argument is not used on Windows) and
// the chunk size after it. CPdxTaskScheduler::CreateTasks takes its serial path when CalcNumTasks() == 1, i.e. when
// that count is 1.
constexpr uintptr_t kRvaDescriptorCtor = sdk::fn::CPdxParallelForDescriptor_Ctor;
constexpr ptrdiff_t kDescriptorTasks = sdk::rt::CPdxParallelForDescriptor_tasks;
// The per-country entry the parallel-for calls on every element: a CCountryAI virtual that checks that the country
// has an AI and is alive and then tail-calls the base update (which runs the AI proper through the next slot and
// more). Hooked per AI class, found through the objects' vtables.
constexpr int kUpdateSlot = sdk::vt::CCountryAI_Update;
constexpr int kMaxCountries = 1024;
constexpr int kMaxTargets = 8;

using Fn = uint64_t (*)(void*, void*, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
Fn g_orig_parallel = nullptr;
Fn g_orig_descriptor = nullptr;
uintptr_t g_base = 0;
bool g_installed = false;
bool g_descriptor_hooked = false;
std::atomic<bool> g_on{ false };
std::atomic<bool> g_nested_inline{ false };
std::atomic<uint64_t> g_nested_seen{ 0 }, g_nested_inlined{ 0 };

// The per-country updates, one hook per AI class seen in the array (hooked on the main thread before a phase starts,
// while the workers are idle). The detour finds its original through the object's own vtable.
struct Target {
    void* fn;
    Fn orig;
};
Target g_targets[kMaxTargets];
int g_ntargets = 0;

// The phase in progress: the array as the engine passed it, the order to process it in, and what each country cost.
// Written by the main thread before the parallel-for starts and after it ends; the workers only read the array and
// the order and write their own country's cost and visited flag.
std::atomic<bool> g_phase{ false };
void* const* g_arr = nullptr;
int g_n = 0;
int g_order[kMaxCountries];                // slot i of the parallel-for processes country g_order[i]
std::atomic<uint8_t> g_visited[kMaxCountries];
uint64_t g_cost[kMaxCountries];           // cycles of this phase, by country index
uint64_t g_start[kMaxCountries];
uint32_t g_tid[kMaxCountries];
void* g_last_ptr[kMaxCountries];           // the previous phase's countries and costs, by pointer
uint64_t g_last_cost[kMaxCountries];
int g_last_n = 0;

std::atomic<uint64_t> g_phases{ 0 }, g_updates{ 0 }, g_remapped{ 0 }, g_unknown{ 0 }, g_anomalies{ 0 };
std::atomic<uint64_t> g_phase_cycles{ 0 }, g_sum_cycles{ 0 }, g_max_cycles{ 0 }, g_max_inflight_sum{ 0 };  // over all phases
std::atomic<int> g_inflight{ 0 }, g_max_inflight{ 0 };
uint64_t g_tsc_per_ms = 0;
std::atomic<uint32_t> g_generation{ 0 };   // bumped when a phase starts
thread_local bool t_in_update = false;     // this thread is inside a country's update of the current phase
uint64_t g_phase_t0 = 0;
int g_timelines_logged = 0;

inline void* SlotFn(const void* obj) { return (*(void* const* const*)obj)[kUpdateSlot]; }

Fn OrigOf(void* fn) {
    for (int i = 0; i < g_ntargets; ++i) {
        if (g_targets[i].fn == fn) return g_targets[i].orig;
    }
    return nullptr;
}

uint64_t UpdateDetour(void* self, void* b, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    if (g_phase.load(std::memory_order_acquire)) {
        int i = -1;
        for (int k = 0; k < g_n; ++k) {
            if (g_arr[k] == self) {
                i = k;
                break;
            }
        }
        if (i >= 0 && g_visited[i].exchange(1, std::memory_order_acq_rel) == 0) {
            const int target = g_on.load(std::memory_order_relaxed) ? g_order[i] : i;
            void* obj = const_cast<void*>(g_arr[target]);
            const Fn orig = OrigOf(SlotFn(obj));
            if (orig) {
                if (target != i) g_remapped.fetch_add(1, std::memory_order_relaxed);
                g_updates.fetch_add(1, std::memory_order_relaxed);
                const int inflight = g_inflight.fetch_add(1, std::memory_order_acq_rel) + 1;
                int mx = g_max_inflight.load(std::memory_order_relaxed);
                while (inflight > mx && !g_max_inflight.compare_exchange_weak(mx, inflight)) {
                }
                const uint64_t t0 = __rdtsc();
                g_start[target] = t0;
                g_tid[target] = GetCurrentThreadId();
                // the heaviest country by yesterday's cost: tell a sampling profiler outside which thread runs it now
                SharedStats* shared = target == g_order[0] && g_n > 1 ? Shared() : nullptr;
                if (shared) {
                    shared->ai_heavy_tid = GetCurrentThreadId();
                    shared->ai_heavy_country = (uint32_t)target;
                    InterlockedExchange((volatile LONG*)&shared->ai_heavy_active, 1);
                }
                t_in_update = true;
                const uint64_t r = orig(obj, b, c, d, e, f, g, h);
                t_in_update = false;
                if (shared) InterlockedExchange((volatile LONG*)&shared->ai_heavy_active, 0);
                g_cost[target] = __rdtsc() - t0;
                g_inflight.fetch_sub(1, std::memory_order_acq_rel);
                return r;
            }
            g_visited[i].store(0, std::memory_order_release);  // cannot process it here: let the engine's call through
        }
        g_anomalies.fetch_add(1, std::memory_order_relaxed);  // not in the array, or a slot reached twice
    }
    const Fn orig = OrigOf(SlotFn(self));
    return orig ? orig(self, b, c, d, e, f, g, h) : 0;
}

// A parallel-for started from inside a country's update: the other workers are busy with their own countries, so
// the forking thread mostly spin-waits for tasks nobody takes; with ai_nested_inline it runs the whole range itself
// (the engine's own serial path for a single task; same results, as every parallel-for's are order-independent).
uint64_t DescriptorDetour(void* self, void* b, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    const uint64_t r = g_orig_descriptor(self, b, c, d, e, f, g, h);
    if (t_in_update) {
        g_nested_seen.fetch_add(1, std::memory_order_relaxed);
        if (g_nested_inline.load(std::memory_order_relaxed)) {
            *(int32_t*)((uint8_t*)self + kDescriptorTasks) = 1;
            g_nested_inlined.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return r;
}

// Hooks the per-country update of every AI class in the array that is not hooked yet (main thread, workers idle).
void HookClasses(void* const* arr, int n) {
    for (int i = 0; i < n; ++i) {
        if (!arr[i]) continue;
        void* fn = SlotFn(arr[i]);
        if (OrigOf(fn)) continue;
        if (g_ntargets == kMaxTargets) return;
        Target& t = g_targets[g_ntargets];
        t.fn = fn;
        if (MH_CreateHook(fn, (LPVOID)&UpdateDetour, (LPVOID*)&t.orig) != MH_OK || MH_EnableHook(fn) != MH_OK) {
            Log("ai: could not hook the per-country update at RVA 0x%llX (vtable 0x%llX)", (unsigned long long)((uintptr_t)fn - g_base),
                (unsigned long long)((uintptr_t)*(void* const*)arr[i] - g_base));
            t.fn = nullptr;
            return;
        }
        ++g_ntargets;
        Log("ai: hooked the per-country update at RVA 0x%llX (vtable 0x%llX)", (unsigned long long)((uintptr_t)fn - g_base),
            (unsigned long long)((uintptr_t)*(void* const*)arr[i] - g_base));
    }
}

uint64_t ParallelDetour(void* arr_, void* n_, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    void* const* arr = (void* const*)arr_;
    const int n = (int)(intptr_t)n_;
    if (n <= 0 || n > kMaxCountries || g_phase.load(std::memory_order_relaxed)) return g_orig_parallel(arr_, n_, c, d, e, f, g, h);
    HookClasses(arr, n);
    g_arr = arr;
    g_n = n;
    // last phase's cost by pointer; a country not seen then ranks first
    uint64_t est[kMaxCountries];
    for (int i = 0; i < n; ++i) {
        est[i] = ~0ull;
        for (int k = 0; k < g_last_n; ++k) {
            if (g_last_ptr[k] == arr[i]) {
                est[i] = g_last_cost[k];
                break;
            }
        }
        if (est[i] == ~0ull) g_unknown.fetch_add(1, std::memory_order_relaxed);
        g_order[i] = i;
        g_visited[i].store(0, std::memory_order_relaxed);
        g_cost[i] = 0;
        g_start[i] = 0;
        g_tid[i] = 0;
    }
    std::stable_sort(g_order, g_order + n, [&](int a, int b) { return est[a] > est[b]; });
    g_max_inflight.store(0, std::memory_order_relaxed);
    g_generation.fetch_add(1, std::memory_order_release);
    g_phase.store(true, std::memory_order_release);
    const uint64_t t0 = __rdtsc();
    g_phase_t0 = t0;
    const uint64_t r = g_orig_parallel(arr_, n_, c, d, e, f, g, h);
    const uint64_t dt = __rdtsc() - t0;
    g_phase.store(false, std::memory_order_release);
    uint64_t sum = 0, mx = 0;
    for (int i = 0; i < n; ++i) {
        g_last_ptr[i] = arr[i];
        g_last_cost[i] = g_cost[i];
        sum += g_cost[i];
        if (g_cost[i] > mx) mx = g_cost[i];
    }
    g_last_n = n;
    const uint64_t phases = g_phases.fetch_add(1, std::memory_order_relaxed) + 1;
    g_phase_cycles.fetch_add(dt, std::memory_order_relaxed);
    g_sum_cycles.fetch_add(sum, std::memory_order_relaxed);
    g_max_cycles.fetch_add(mx, std::memory_order_relaxed);
    g_max_inflight_sum.fetch_add((uint64_t)g_max_inflight.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (g_timelines_logged < 2 && (phases == 30 || phases == 60)) {
        ++g_timelines_logged;
        Log("ai: phase %llu timeline: %d countries, %d in flight at most, phase %.2f ms, heaviest %.2f ms, sum %.2f ms, order %s (main thread %lu)",
            (unsigned long long)phases, n, g_max_inflight.load(), (double)dt / (double)g_tsc_per_ms, (double)mx / (double)g_tsc_per_ms,
            (double)sum / (double)g_tsc_per_ms, g_on.load() ? "heaviest first" : "the array's", GetCurrentThreadId());
        for (int i = 0; i < n; ++i) {
            if (!g_tid[i]) continue;
            Log("ai:   country %3d  vtable 0x%llX  start %7.2f ms  %7.2f ms  thread %lu", i,
                (unsigned long long)((uintptr_t)*(void* const*)arr[i] - g_base), (double)(g_start[i] - g_phase_t0) / (double)g_tsc_per_ms,
                (double)g_cost[i] / (double)g_tsc_per_ms, (unsigned long)g_tid[i]);
        }
    }
    return r;
}

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const uint64_t t0 = __rdtsc();
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    do {
        QueryPerformanceCounter(&q1);
    } while ((q1.QuadPart - q0.QuadPart) * 1000 < f.QuadPart * 20);
    g_tsc_per_ms = (__rdtsc() - t0) / 20;
    const LPVOID par = (LPVOID)(base + kRvaUpdateAIParallel);
    if (MH_CreateHook(par, (LPVOID)&ParallelDetour, (LPVOID*)&g_orig_parallel) != MH_OK || MH_EnableHook(par) != MH_OK) {
        Log("ai: could not hook UpdateAIParallel; ai_parallel_lpt unavailable");
        return false;
    }
    g_installed = true;
    Log("ai: hooked UpdateAIParallel (ai_parallel_lpt); the per-country updates are hooked per AI class when first seen");
    {
        const LPVOID ctor = (LPVOID)(base + kRvaDescriptorCtor);
        if (MH_CreateHook(ctor, (LPVOID)&DescriptorDetour, (LPVOID*)&g_orig_descriptor) != MH_OK || MH_EnableHook(ctor) != MH_OK) {
            Log("ai: could not hook CPdxParallelForDescriptor's constructor; ai_nested_inline unavailable");
        } else {
            g_descriptor_hooked = true;
            Log("ai: hooked CPdxParallelForDescriptor's constructor (ai_nested_inline)");
        }
    }
    return true;
}

void SetNestedInline(bool on) {
    if (!g_descriptor_hooked) return;
    if (g_nested_inline.exchange(on) != on) Log("ai_nested_inline=%d", on ? 1 : 0);
}

void SetEnabled(bool on) {
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("ai_parallel_lpt=%d", on ? 1 : 0);
}

bool InCountryUpdate() { return t_in_update; }
uint32_t PhaseGeneration() { return g_generation.load(std::memory_order_acquire); }

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "ai_parallel_lpt=unavailable";
    const uint64_t phases = g_phases.load();
    const double per = phases ? 1.0 / (double)phases / (double)g_tsc_per_ms : 0.0;
    char buf[400];
    snprintf(buf, sizeof buf,
             "ai_parallel_lpt=%s(phases=%llu classes=%d countries/phase=%.1f remapped=%llu unknown=%llu anomalies=%llu phase=%.2fms heaviest=%.2fms sum=%.1fms inflight_max=%.1f)"
             " | ai_nested_inline=%s(nested=%llu inlined=%llu)",
             g_on.load() ? "on" : "off", (unsigned long long)phases, g_ntargets, phases ? (double)g_updates.load() / (double)phases : 0.0,
             (unsigned long long)g_remapped.load(), (unsigned long long)g_unknown.load(), (unsigned long long)g_anomalies.load(),
             (double)g_phase_cycles.load() * per, (double)g_max_cycles.load() * per, (double)g_sum_cycles.load() * per,
             phases ? (double)g_max_inflight_sum.load() / (double)phases : 0.0,
             g_descriptor_hooked ? (g_nested_inline.load() ? "on" : "off") : "unavailable", (unsigned long long)g_nested_seen.load(),
             (unsigned long long)g_nested_inlined.load());
    return std::string(buf);
}

}  // namespace perf::ai
