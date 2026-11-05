#include "perf_weight.hpp"
#include "stellaris_sdk.hpp"
#include "perf_ai.hpp"
#include "perf_tweaks.hpp"
#include "MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace perf::weight {
namespace {

// CMeanTimeToHappen::GetRawFactor(this, CFixedPoint* out, const CEventScope* scope) -> out, from the SDK: the base
// factor is written into *out and every modifier of the list scales it.
constexpr uintptr_t kRvaGetRawFactor = sdk::fn::CMeanTimeToHappen_GetRawFactor;

using Fn = void* (*)(void*, void*, const void*);
Fn g_orig = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{ false };

// Per-thread 4-way set-associative table, keyed by a 64-bit hash of (weight, scope identity), valid for one phase.
struct Entry {
    uint64_t hash;
    uint64_t value;
    uint32_t gen;
    uint32_t pad;
};
struct alignas(32) Set {
    Entry way[4];
};
constexpr size_t kSets = 2048;  // 192 KB per thread
thread_local Set* t_sets = nullptr;

std::atomic<uint64_t> g_calls{ 0 }, g_bypass{ 0 }, g_hits{ 0 }, g_misses{ 0 };

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

uint64_t Hash(const void* weight, const ScopeKey& k) {
    uint64_t h = (uint64_t)weight;
    for (int i = 0; i < 4; ++i) {
        h = Mix(h, k.type[i] ^ ((uint64_t)k.id[i] << 20));
        h = Mix(h, k.local[i]);
    }
    return h ^ (h >> 33);
}

void* Detour(void* weight, void* out, const void* scope) {
    if (!g_on.load(std::memory_order_relaxed) || !ai::InCountryUpdate()) return g_orig(weight, out, scope);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    ScopeKey key;
    if (!BuildScopeKey(scope, &key)) {
        g_bypass.fetch_add(1, std::memory_order_relaxed);
        return g_orig(weight, out, scope);
    }
    if (!t_sets) t_sets = (Set*)RegisterTable(kSets * sizeof(Set));
    if (!t_sets) return g_orig(weight, out, scope);
    const uint32_t gen = ai::PhaseGeneration();
    const uint64_t hash = Hash(weight, key);
    Set& set = t_sets[(hash >> 7) & (kSets - 1)];
    Entry* victim = &set.way[hash & 3];
    for (Entry& e : set.way) {
        if (e.gen != gen) {
            victim = &e;
        } else if (e.hash == hash) {
            g_hits.fetch_add(1, std::memory_order_relaxed);
            *(uint64_t*)out = e.value;
            return out;
        }
    }
    g_misses.fetch_add(1, std::memory_order_relaxed);
    void* r = g_orig(weight, out, scope);
    victim->hash = hash;
    victim->gen = gen;
    victim->value = *(const uint64_t*)out;
    return r;
}

}  // namespace

bool Install(uintptr_t base) {
    const LPVOID target = (LPVOID)(base + kRvaGetRawFactor);
    if (MH_CreateHook(target, (LPVOID)&Detour, (LPVOID*)&g_orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("weight: could not hook CMeanTimeToHappen::GetRawFactor; ai_weight_cache unavailable");
        return false;
    }
    g_installed = true;
    Log("weight: hooked CMeanTimeToHappen::GetRawFactor (ai_weight_cache)");
    return true;
}

void SetEnabled(bool on) {
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("ai_weight_cache=%d", on ? 1 : 0);
}

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "ai_weight_cache=unavailable";
    const uint64_t calls = g_calls.load(), hits = g_hits.load();
    char buf[160];
    snprintf(buf, sizeof buf, "ai_weight_cache=%s(calls=%llu hits=%llu misses=%llu bypass=%llu hit_rate=%.1f%%)", g_on.load() ? "on" : "off",
             (unsigned long long)calls, (unsigned long long)hits, (unsigned long long)g_misses.load(), (unsigned long long)g_bypass.load(),
             calls ? 100.0 * (double)hits / (double)calls : 0.0);
    return std::string(buf);
}

}  // namespace perf::weight
