#include "perf_gui.hpp"
#include "perf_engine.hpp"
#include "perf_input.hpp"
#include "perf_tweaks.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace perf::gui {
namespace {

// The GUI's call sites: a detour is entered by a jump from the function's start, so its return address is the caller's.
constexpr uintptr_t kUpgradeReturn = sdk::site::CFleetActionUpgradeButton_IsValid_CanAffordUpgrade + 5;
constexpr uintptr_t kCursorReturn = sdk::site::CInGameIdler_UpdateCursor_CalcCanMoveTo + 5;
constexpr uintptr_t kHoverReturn = sdk::site::CGalacticObjectMapIcon_UpdateHoverIndicator_CalcCanMoveTo + 5;
// A CPdxArray member's count, as far behind its data pointer as the flag containers' is behind theirs.
constexpr ptrdiff_t kArrayCount = sdk::rt::CPdxIntegerFlags_count - sdk::rt::CPdxIntegerFlags_ids;

constexpr uint64_t kSettleMs = 100;     // a press's effect reaches the game state a frame or two later
constexpr uint64_t kUpgradeAgeMs = 1000;
constexpr uint64_t kPathAgeMs = 500;

using FnAfford = bool (*)(const void* fleet, void* reason);
using FnCanMove = bool (*)(const void* movement_manager, const void* system, void* ai_cache, void* reason);
using FnAssign = void* (*)(void* cstring, const char* chars, size_t length);

uintptr_t g_base = 0;
bool g_installed = false;
FnAfford g_orig_afford = nullptr;
FnCanMove g_orig_can_move = nullptr;
std::atomic<int> g_upgrade_mode{ 0 }, g_path_mode{ 0 };
std::atomic<DWORD> g_thread{ 0 };  // the caches belong to the thread that runs the GUI
std::atomic<uint64_t> g_upgrade_kept{ 0 }, g_upgrade_asked{ 0 }, g_upgrade_mismatches{ 0 }, g_upgrade_cycles{ 0 };
std::atomic<uint64_t> g_path_kept{ 0 }, g_path_asked{ 0 }, g_path_mismatches{ 0 }, g_path_cycles{ 0 };

bool OwnThread() {
    const DWORD tid = GetCurrentThreadId();
    DWORD owner = g_thread.load(std::memory_order_relaxed);
    if (owner == 0 && g_thread.compare_exchange_strong(owner, tid)) owner = tid;
    return owner == tid;
}

// ---- the upgrade button's affordability check ----------------------------------------------------------------------

struct UpgradeEntry {
    bool valid = false;
    const void* fleet = nullptr;
    uint64_t ships = 0;  // the fleet's ship array and count: a merge, split or loss asks again
    int32_t count = 0;
    bool with_reason = false;
    uint64_t generation = 0;
    uint64_t actions = 0;  // the action epoch it was computed in
    uint64_t built = 0;
    bool result = false;
    std::string reason;
};
constexpr size_t kUpgradeSlots = 16;  // one per selected fleet's entry
UpgradeEntry g_upgrade[kUpgradeSlots];

bool ReadShips(const void* fleet, uint64_t* ships, int32_t* count) {
    __try {
        *ships = *(const uint64_t*)((const char*)fleet + sdk::ent::CFleet::ships);
        *count = *(const int32_t*)((const char*)fleet + sdk::ent::CFleet::ships + kArrayCount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool AffordTimed(const void* fleet, void* reason) {
    const uint64_t t0 = __rdtsc();
    const bool result = g_orig_afford(fleet, reason);
    g_upgrade_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_upgrade_asked.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool AffordDetour(const void* fleet, void* reason) {
    const int mode = g_upgrade_mode.load(std::memory_order_relaxed);
    if (mode == 0 || (uintptr_t)_ReturnAddress() != g_base + kUpgradeReturn) return g_orig_afford(fleet, reason);
    EngineString* text = (EngineString*)reason;
    uint64_t ships;
    int32_t count;
    // a reason that already holds text is appended to or replaced, whichever the engine does: left to the engine
    if (!OwnThread() || (text && !text->text.empty()) || !ReadShips(fleet, &ships, &count)) {
        return AffordTimed(fleet, reason);
    }
    const uint64_t now = GetTickCount64();
    const input::Actions actions = input::Sample(now);  // a press or release may have changed what the fleet can afford
    const uint64_t generation = Generation();
    UpgradeEntry& e = g_upgrade[((uintptr_t)fleet >> 6) % kUpgradeSlots];
    const bool settled = now - actions.changed_ms >= kSettleMs;
    const bool same = e.valid && e.fleet == fleet && e.ships == ships && e.count == count && e.with_reason == (text != nullptr) &&
                      e.generation == generation && e.actions == actions.epoch;
    if (same && settled && now - e.built < kUpgradeAgeMs) {
        if (mode != 2) {
            if (text) ((FnAssign)(g_base + sdk::fn::CString_Assign))(text, e.reason.data(), e.reason.size());
            g_upgrade_kept.fetch_add(1, std::memory_order_relaxed);
            return e.result;
        }
        const bool result = AffordTimed(fleet, reason);
        const bool equal = result == e.result && (!text || (text->text.size() == e.reason.size() &&
                                                            memcmp(text->text.data(), e.reason.data(), e.reason.size()) == 0));
        if (equal) {
            g_upgrade_kept.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (g_upgrade_mismatches.fetch_add(1, std::memory_order_relaxed) < 12) {
                Log("fleet_upgrade_cache verify: the kept answer (%d, %zu bytes of reason, %llu ms old) is not the engine's (%d, %zu bytes)",
                    (int)e.result, e.reason.size(), (unsigned long long)(now - e.built), (int)result, text ? text->text.size() : (size_t)0);
            }
            e.result = result;
            if (text) e.reason.assign(text->text.data(), text->text.size());
            e.built = now;
        }
        return result;
    }
    const bool result = AffordTimed(fleet, reason);
    e.valid = settled;  // an answer computed right after an input may predate the input's effect: it is not kept
    if (!settled) return result;
    e.fleet = fleet;
    e.ships = ships;
    e.count = count;
    e.with_reason = text != nullptr;
    e.generation = generation;
    e.actions = actions.epoch;
    e.built = now;
    e.result = result;
    if (text) {
        e.reason.assign(text->text.data(), text->text.size());
    } else {
        e.reason.clear();
    }
    return result;
}

// ---- the cursor's and the hover indicator's path check --------------------------------------------------------------

struct PathEntry {
    const void* manager = nullptr;  // the fleet's movement manager
    const void* system = nullptr;
    uint64_t generation = 0;
    uint64_t built = 0;  // 0 = empty
    bool result = false;
};
constexpr size_t kPathSlots = 64;  // a selection of many fleets asks once per fleet
PathEntry g_path[kPathSlots];

bool CanMoveTimed(const void* manager, const void* system, void* ai_cache, void* reason) {
    const uint64_t t0 = __rdtsc();
    const bool result = g_orig_can_move(manager, system, ai_cache, reason);
    g_path_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    g_path_asked.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool CanMoveDetour(const void* manager, const void* system, void* ai_cache, void* reason) {
    const int mode = g_path_mode.load(std::memory_order_relaxed);
    if (mode == 0) return g_orig_can_move(manager, system, ai_cache, reason);
    const uintptr_t from = (uintptr_t)_ReturnAddress() - g_base;
    if ((from != kCursorReturn && from != kHoverReturn) || ai_cache || reason) return g_orig_can_move(manager, system, ai_cache, reason);
    if (!OwnThread()) return CanMoveTimed(manager, system, ai_cache, reason);
    const uint64_t now = GetTickCount64();
    const uint64_t generation = Generation();
    PathEntry& e = g_path[(((uintptr_t)manager >> 4) ^ ((uintptr_t)system >> 4) * 0x9E3779B1u) % kPathSlots];
    if (e.built && e.manager == manager && e.system == system && e.generation == generation && now - e.built < kPathAgeMs) {
        if (mode != 2) {
            g_path_kept.fetch_add(1, std::memory_order_relaxed);
            return e.result;
        }
        const bool result = CanMoveTimed(manager, system, ai_cache, reason);
        if (result == e.result) {
            g_path_kept.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (g_path_mismatches.fetch_add(1, std::memory_order_relaxed) < 12) {
                Log("cursor_path_cache verify: the kept answer %d (%llu ms old) is not the engine's %d", (int)e.result,
                    (unsigned long long)(now - e.built), (int)result);
            }
            e.result = result;
            e.built = now;
        }
        return result;
    }
    const bool result = CanMoveTimed(manager, system, ai_cache, reason);
    e.manager = manager;
    e.system = system;
    e.generation = generation;
    e.built = now ? now : 1;
    e.result = result;
    return result;
}

bool Hook(uintptr_t rva, void* detour, void** original, const char* name) {
    void* target = (void*)(g_base + rva);
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        Log("gui: could not hook %s at RVA 0x%llX", name, (unsigned long long)rva);
        return false;
    }
    Log("gui: hooked %s at RVA 0x%llX", name, (unsigned long long)rva);
    return true;
}

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
    const bool a = Hook(sdk::fn::NFleetUtil_CanAffordUpgrade, (void*)&AffordDetour, (void**)&g_orig_afford, "NFleetUtil::CanAffordUpgrade");
    const bool b = Hook(sdk::fn::CFleetMovementManager_CalcCanMoveTo, (void*)&CanMoveDetour, (void**)&g_orig_can_move,
                        "CFleetMovementManager::CalcCanMoveTo");
    g_installed = a && b;
    return g_installed;
}

void SetUpgradeMode(int mode) { g_upgrade_mode.store(g_orig_afford && mode >= 0 && mode <= 2 ? mode : 0); }
void SetPathMode(int mode) { g_path_mode.store(g_orig_can_move && mode >= 0 && mode <= 2 ? mode : 0); }
bool Installed() { return g_installed; }

Counters Read() {
    return { g_upgrade_kept.load(), g_upgrade_asked.load(), g_upgrade_mismatches.load(), g_upgrade_cycles.load(),
             g_path_kept.load(), g_path_asked.load(), g_path_mismatches.load(), g_path_cycles.load() };
}

std::string Stats(double ms_per_cycle) {
    const Counters c = Read();
    auto part = [&](const char* name, bool hooked, int mode, uint64_t kept, uint64_t asked, uint64_t mismatches, uint64_t cycles) {
        if (!hooked) return std::string(name) + "=unavailable";
        char buf[200];
        int n = snprintf(buf, sizeof buf, "%s=%d kept=%llu asked=%llu", name, mode, (unsigned long long)kept, (unsigned long long)asked);
        if (asked && ms_per_cycle > 0) n += snprintf(buf + n, sizeof buf - n, " (%.3f ms each)", (double)cycles * ms_per_cycle / (double)asked);
        if (mode == 2 || mismatches) snprintf(buf + n, sizeof buf - n, " mismatches=%llu", (unsigned long long)mismatches);
        return std::string(buf);
    };
    return part("fleet_upgrade_cache", g_orig_afford != nullptr, g_upgrade_mode.load(), c.upgrade_kept, c.upgrade_asked, c.upgrade_mismatches,
                c.upgrade_cycles) +
           " | " +
           part("cursor_path_cache", g_orig_can_move != nullptr, g_path_mode.load(), c.path_kept, c.path_asked, c.path_mismatches, c.path_cycles);
}

}  // namespace perf::gui
