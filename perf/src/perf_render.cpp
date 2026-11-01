#include "perf_render.hpp"
#include "perf_tweaks.hpp"
#include "perf_ui.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace perf::render {
namespace {

using RenderFn = void (*)(void*, void*, void*, void*);
RenderFn g_orig = nullptr;
uint8_t* g_target = nullptr;
std::atomic<int> g_fps{ 0 };
std::atomic<int> g_in_tick{ 0 };
std::atomic<uint64_t> g_dropped{ 0 }, g_passed{ 0 };
uint64_t g_last = 0;      // TSC of the last frame let through inside a tick (main thread only)
double g_tsc_per_sec = 0;  // measured when the hook is installed

// The call sites of RenderFrame all look like this (RenderFrameIfNeeded inlined; 35 sites):
//   cmp byte ptr [reg+5D0h], 0 ; je ; cmp byte ptr [rip+g_bFrameSmoothing], 0 ; je ; cmp dword ptr [rip+g_nTicksPerTurn], 1 ; jne ;
//   cmp byte ptr [reg+0A0h], 0 ; je ; call RenderFrame
// (the registers and the optional REX prefix differ between sites).
bool IsSite(const uint8_t* p, size_t* len) {
    if (!(p[0] == 0x80 && p[1] >= 0x80 && p[1] <= 0xBF && p[2] == 0xD0 && p[3] == 0x05 && p[4] == 0 && p[5] == 0 && p[6] == 0)) return false;
    if (p[7] != 0x74) return false;
    const uint8_t* q = p + 9;
    if (!(q[0] == 0x80 && q[1] == 0x3D && q[6] == 0x00 && q[7] == 0x74)) return false;
    q += 9;
    if (!(q[0] == 0x83 && q[1] == 0x3D && q[6] == 0x01 && q[7] == 0x75)) return false;
    q += 9;
    if (q[0] == 0x41) ++q;
    if (!(q[0] == 0x80 && q[1] >= 0xB8 && q[1] <= 0xBF && q[2] == 0xA0 && q[3] == 0 && q[4] == 0 && q[5] == 0 && q[6] == 0x00 && q[7] == 0x74)) return false;
    q += 9;
    if (q[0] != 0xE8) return false;
    *len = (size_t)(q + 5 - p);
    return true;
}

void RenderDetour(void* a, void* b, void* c, void* d) {
    const int fps = g_fps.load(std::memory_order_relaxed);
    if (fps > 0 && (g_in_tick.load(std::memory_order_relaxed) > 0 || ui::CapAll())) {
        // the engine's own timer stays "due" while frames are dropped, so this is asked at every check: rdtsc, not QPC
        const uint64_t now = __rdtsc();
        if ((double)(now - g_last) < g_tsc_per_sec / fps) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_last = now;
    }
    g_passed.fetch_add(1, std::memory_order_relaxed);
    g_orig(a, b, c, d);
}

}  // namespace

bool Install(uintptr_t base) {
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t *b = nullptr, *e = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            b = (const uint8_t*)(base + sec[i].VirtualAddress);
            e = b + sec[i].Misc.VirtualSize;
        }
    }
    if (!b) return false;
    const uint8_t* target = nullptr;
    int sites = 0;
    for (const uint8_t* p = b; p + 60 < e; ++p) {
        size_t len;
        if (p[0] != 0x80 || !IsSite(p, &len)) continue;
        const uint8_t* call = p + len - 5;
        const uint8_t* t = call + 5 + *(const int32_t*)(call + 1);
        if (t < b || t >= e) continue;
        if (target && t != target) {
            Log("render: the RenderFrame call sites do not agree on one function; the cap stays off");
            return false;
        }
        target = t;
        ++sites;
    }
    if (sites < 5 || !target) {
        Log("render: found %d RenderFrame call sites (expected about 35); the cap stays off", sites);
        return false;
    }
    {   // TSC rate against QPC (the TSC is invariant on every CPU this game runs on)
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        const uint64_t t0 = __rdtsc();
        Sleep(50);
        QueryPerformanceCounter(&q1);
        const uint64_t t1 = __rdtsc();
        g_tsc_per_sec = (double)(t1 - t0) * (double)f.QuadPart / (double)(q1.QuadPart - q0.QuadPart);
    }
    g_target = (uint8_t*)target;
    if (MH_CreateHook(g_target, (LPVOID)&RenderDetour, (LPVOID*)&g_orig) != MH_OK || MH_EnableHook(g_target) != MH_OK) {
        Log("render: could not hook RenderFrame; the cap stays off");
        g_target = nullptr;
        return false;
    }
    Log("render: RenderFrame at RVA 0x%llX (%d call sites)", (unsigned long long)((uintptr_t)target - base), sites);
    return true;
}

void SetFps(int fps) { g_fps.store(fps < 0 ? 0 : fps); }
void TickBegin() { g_in_tick.fetch_add(1); }
void TickEnd() { g_in_tick.fetch_sub(1); }
bool Installed() { return g_target != nullptr; }
uint64_t Dropped() { return g_dropped.load(); }
uint64_t Passed() { return g_passed.load(); }

}  // namespace perf::render
