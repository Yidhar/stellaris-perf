#include "perf_jobs.hpp"
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

uint64_t Detour(void* a, void* b, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    for (int i = 0; i < kPairs; ++i) {
        if (ret == g_second[i]) {
            g_second_calls.fetch_add(1, std::memory_order_relaxed);
            if (g_on.load(std::memory_order_relaxed)) {
                g_skipped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            break;
        }
        if (ret == g_first[i]) {
            g_first_calls.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return g_orig(a, b, c, d, e, f, g, h);
}

bool CallsTo(uintptr_t at, uintptr_t target) {
    const uint8_t* p = (const uint8_t*)(g_base + at);
    return p[0] == 0xE8 && at + 5 + (uintptr_t)(int64_t)*(const int32_t*)(p + 1) == target;
}

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
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
    return true;
}

void SetEnabled(bool on) {
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("pop_jobs_single_pass=%d", on ? 1 : 0);
}

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "pop_jobs_single_pass=unavailable";
    char buf[160];
    snprintf(buf, sizeof buf, "pop_jobs_single_pass=%s(first=%llu second=%llu skipped=%llu)", g_on.load() ? "on" : "off",
             (unsigned long long)g_first_calls.load(), (unsigned long long)g_second_calls.load(), (unsigned long long)g_skipped.load());
    return std::string(buf);
}

}  // namespace perf::jobs
