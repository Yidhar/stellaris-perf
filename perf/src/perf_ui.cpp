#include "perf_ui.hpp"
#include "perf_tweaks.hpp"
#include "perf_phase.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace perf::ui {
namespace {

constexpr uintptr_t kRvaFrame = sdk::fn::CGameIdler_Idle;  // CGameIdler::Idle(bool), from the SDK

using FrameFn = uint64_t (*)(void*, void*, void*, void*);
FrameFn g_orig = nullptr;
uint8_t* g_target = nullptr;
std::atomic<bool> g_probe{ false };
std::atomic<bool> g_cap_all{ false };
double g_tsc_per_sec = 0;
uint64_t g_last_tick_end = 0;  // TSC of the last finished tick (main thread)

// probe state, main thread only
uint64_t g_win_start = 0, g_frames = 0, g_frame_cycles = 0, g_ticks = 0, g_tick_cycles = 0, g_tick_begin = 0;
int g_depth = 0;

void Report(uint64_t now) {
    const double secs = (double)(now - g_win_start) / g_tsc_per_sec;
    const double frame_ms = g_frames ? 1000.0 * (double)g_frame_cycles / g_tsc_per_sec / (double)g_frames : 0;
    const double tick_ms = g_ticks ? 1000.0 * (double)g_tick_cycles / g_tsc_per_sec / (double)g_ticks : 0;
    Log("ui probe: %.1f s | frame fn %llu calls (%.1f/s, %.2f ms each, %.1f%% of time) | ticks %llu (%.1f/s, %.2f ms each, %.1f%% of time)",
        secs, (unsigned long long)g_frames, g_frames / secs, frame_ms, 100.0 * (double)g_frame_cycles / g_tsc_per_sec / secs,
        (unsigned long long)g_ticks, g_ticks / secs, tick_ms, 100.0 * (double)g_tick_cycles / g_tsc_per_sec / secs);
    g_win_start = now;
    g_frames = g_frame_cycles = g_ticks = g_tick_cycles = 0;
}

uint64_t FrameProbe(void* a, void* b, void* c, void* d) {
    if (!g_probe.load(std::memory_order_relaxed) || g_depth > 0) return g_orig(a, b, c, d);
    ++g_depth;
    const uint64_t t0 = __rdtsc();
    if (!g_win_start) g_win_start = t0;
    const uint64_t r = g_orig(a, b, c, d);
    const uint64_t t1 = __rdtsc();
    --g_depth;
    ++g_frames;
    g_frame_cycles += t1 - t0;
    if ((double)(t1 - g_win_start) > 10.0 * g_tsc_per_sec) Report(t1);
    return r;
}

// The phase timing (perf_phase.hpp) sees the frame function as one phase; the nested call made by RenderFrame is
// swallowed there, so it counts as rendering.
uint64_t FrameDetour(void* a, void* b, void* c, void* d) {
    const bool ph = phase::Enter(phase::kFrame);
    const uint64_t r = FrameProbe(a, b, c, d);
    phase::Exit(ph);
    return r;
}

}  // namespace

bool Install(uintptr_t base) {
    {
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        const uint64_t t0 = __rdtsc();
        Sleep(50);
        QueryPerformanceCounter(&q1);
        const uint64_t t1 = __rdtsc();
        g_tsc_per_sec = (double)(t1 - t0) * (double)f.QuadPart / (double)(q1.QuadPart - q0.QuadPart);
    }
    g_target = (uint8_t*)(base + kRvaFrame);
    if (MH_CreateHook(g_target, (LPVOID)&FrameDetour, (LPVOID*)&g_orig) != MH_OK || MH_EnableHook(g_target) != MH_OK) {
        Log("ui: could not hook the frame function");
        g_target = nullptr;
        return false;
    }
    Log("ui: frame function hooked at RVA 0x%llX", (unsigned long long)kRvaFrame);
    return true;
}

void SetProbe(bool on) { g_probe.store(on); g_win_start = 0; }
void SetCapAll(bool on) { g_cap_all.store(on); }

bool CapAll() {
    // while the simulation is running (a tick ended within the last 250 ms); paused, in a menu or loading nothing is capped
    if (!g_cap_all.load(std::memory_order_relaxed) || !g_tsc_per_sec) return false;
    return (double)(__rdtsc() - g_last_tick_end) < 0.25 * g_tsc_per_sec;
}

void TickBegin() { if (g_probe.load(std::memory_order_relaxed)) g_tick_begin = __rdtsc(); }
void TickEnd() {
    g_last_tick_end = __rdtsc();
    if (!g_probe.load(std::memory_order_relaxed) || !g_tick_begin) return;
    ++g_ticks;
    g_tick_cycles += g_last_tick_end - g_tick_begin;
    g_tick_begin = 0;
}
bool Installed() { return g_target != nullptr; }

}  // namespace perf::ui
