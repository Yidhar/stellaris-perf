#include "perf_tooltip.hpp"
#include "perf_engine.hpp"
#include "perf_input.hpp"
#include "perf_phase.hpp"
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

namespace perf::tooltip {
namespace {

// CTooltipManager::Update's two provider calls (the SDK finds them and reads their slots):
//   call qword ptr [rax + kHandlerSlot*8]   rcx = the hovered CGuiObject; rax = its tooltip handler, or null. A handler
//                                           is used once, right after: `call qword ptr [rax]`, GetToolTip(handler,
//                                           object, tip)
//   call qword ptr [rax + kBuildSlot*8]     rcx = the idler, rdx = the hovered object or null, r8 = tip
constexpr uintptr_t kHandlerSite = sdk::site::CTooltipManager_Update_GetHandler;
constexpr uintptr_t kIdlerSite = sdk::site::CTooltipManager_Update_BuildToolTip;
constexpr size_t kHandlerSlot = sdk::vt::CGuiObject_GetToolTipHandler;
constexpr size_t kBuildSlot = sdk::vt::CGameIdler_BuildToolTip;
constexpr size_t kSiteSize = 6;  // FF 90 disp32: one instruction a 5-byte jump fits in, with an intact one behind it

// CToolTip: kStrings CStrings, then plain state up to its end (no pointers: two ints and two flags)
constexpr size_t kStrings = (size_t)sdk::rt::CToolTip_string_count;
constexpr size_t kStringsAt = (size_t)sdk::rt::CToolTip_strings;
constexpr size_t kStateAt = (size_t)sdk::rt::CToolTip_state;
constexpr size_t kStateSize = (size_t)sdk::rt::CToolTip_size - kStateAt;
static_assert(kStringsAt + kStrings * sizeof(EngineString) == kStateAt, "CToolTip: the state does not follow its strings");

// An input's effect reaches the game state a frame or two later (a command is posted, then executed): right after
// an input nothing is kept.
constexpr DWORD kSettleMs = 100;

using FnGetHandler = void* (*)(void* object);
using FnMousePosition = int32_t* (*)(void* input, int32_t* point);
using FnProvide = bool (*)(void* provider, void* object, void* tip);  // the handler's slot 0 and the idler's BuildToolTip
using FnAssign = void* (*)(void* cstring, const char* chars, size_t length);

uintptr_t g_base = 0;
FnAssign g_assign = nullptr;  // the engine's CString assign
bool g_installed = false;
std::atomic<int> g_mode{ 0 };
std::atomic<int> g_max_age_ms{ 250 };
std::atomic<uint64_t> g_bypassed{ 0 }, g_mismatches{ 0 };

// Per call site: answers kept, providers asked, and the time inside the provider.
struct SiteStats {
    std::atomic<uint64_t> kept{ 0 }, built{ 0 }, cycles{ 0 };
};
SiteStats g_handler_stats, g_idler_stats;

// What a provider left in the tooltip, and what it returned.
struct Snapshot {
    std::string text[kStrings];
    uint8_t state[kStateSize] = {};
    bool result = false;
};

// One per call site: the last answer and what it was an answer to.
struct Entry {
    bool valid = false;
    const void* provider = nullptr;  // the handler, or the idler
    const void* function = nullptr;  // the virtual that was asked
    const void* object = nullptr;    // the hovered object
    const void* object_class = nullptr;
    size_t before[kStrings] = {};    // the tooltip's text sizes when the provider was asked (the handler may leave some)
    uint64_t generation = 0;         // game day and settings
    DWORD input = 0;                 // the last key or button press when it was built
    POINT cursor = {};
    uint64_t built = 0;              // GetTickCount64
    Snapshot snap;
};
Entry g_handler_entry, g_idler_entry;  // only the thread running the tooltip manager touches them
std::atomic<DWORD> g_thread{ 0 };

const EngineString* TextOf(const void* tip, size_t i) {
    return (const EngineString*)((const char*)tip + kStringsAt + i * sizeof(EngineString));
}

void Capture(const void* tip, bool result, Snapshot* s) {
    for (size_t i = 0; i < kStrings; ++i) {
        const std::string& t = TextOf(tip, i)->text;
        s->text[i].assign(t.data(), t.size());
    }
    memcpy(s->state, (const char*)tip + kStateAt, kStateSize);
    s->result = result;
}

// The engine's own assign, so the buffers stay the engine allocator's.
void Restore(void* tip, const Snapshot& s) {
    for (size_t i = 0; i < kStrings; ++i) {
        g_assign((char*)tip + kStringsAt + i * sizeof(EngineString), s.text[i].data(), s.text[i].size());
    }
    memcpy((char*)tip + kStateAt, s.state, kStateSize);
}

bool Same(const void* tip, bool result, const Snapshot& s) {
    if (result != s.result || memcmp((const char*)tip + kStateAt, s.state, kStateSize) != 0) return false;
    for (size_t i = 0; i < kStrings; ++i) {
        const std::string& t = TextOf(tip, i)->text;
        if (t.size() != s.text[i].size() || memcmp(t.data(), s.text[i].data(), t.size()) != 0) return false;
    }
    return true;
}

// Verify mode: where the provider's answer and the cached one part (the first few times).
void Report(const char* site, const void* tip, bool result, const Snapshot& s, uint64_t age_ms) {
    static std::atomic<int> reported{ 0 };
    if (reported.fetch_add(1) >= 24) return;
    auto printable = [](const std::string& t, size_t from, char* out, size_t cap) {
        size_t n = 0;
        for (size_t k = from; k < t.size() && n + 1 < cap; ++k) {
            const unsigned char c = (unsigned char)t[k];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        out[n] = 0;
    };
    if (result != s.result) {
        Log("tooltip verify (%s, %llu ms old): the provider now returns %d, the cached answer was %d", site,
            (unsigned long long)age_ms, (int)result, (int)s.result);
        return;
    }
    for (size_t i = 0; i < kStrings; ++i) {
        const std::string now(TextOf(tip, i)->text.data(), TextOf(tip, i)->text.size());
        const std::string& old = s.text[i];
        if (now == old) continue;
        size_t at = 0;
        while (at < now.size() && at < old.size() && now[at] == old[at]) ++at;
        char a[72], b[72];
        const size_t from = at > 24 ? at - 24 : 0;
        printable(old, from, a, sizeof a);
        printable(now, from, b, sizeof b);
        Log("tooltip verify (%s, %llu ms old): text %zu differs at byte %zu (cached %zu bytes, now %zu): cached \"%s\" now \"%s\"", site,
            (unsigned long long)age_ms, i, at, old.size(), now.size(), a, b);
        return;
    }
    Log("tooltip verify (%s, %llu ms old): the state behind the texts differs", site, (unsigned long long)age_ms);
}

// The time of the last key or button press (perf_input.hpp), the mouse position as the engine itself reads it (the
// idler's input object, the getter CTooltipManager::Update calls first) and the clock; false without an idler.
bool SampleInput(unsigned long* last_input, long* cursor_x, long* cursor_y, uint64_t* now_ms) {
    void* idler = *(void* const*)(g_base + sdk::glob::g_CurrentIdler);
    void* device = idler ? *(void* const*)((const char*)idler + sdk::rt::CGameIdler_input) : nullptr;
    if (!device) return false;
    int32_t point[2] = {};
    (*(FnMousePosition* const*)device)[sdk::vt::CInput_GetMousePosition](device, point);
    *cursor_x = point[0];
    *cursor_y = point[1];
    *now_ms = GetTickCount64();
    *last_input = (unsigned long)input::Sample(*now_ms).changed_ms;
    return true;
}
detail::Sampler g_sample = &SampleInput;

template <class Ask>
bool Answer(Entry& e, SiteStats& stats, const char* site, const void* provider, const void* function, void* object, void* tip, Ask call) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0) return call();
    auto ask = [&] {
        const phase::GuiContext gui;  // a provider is GUI code: the fleet power it asks for comes from the GUI's cache
        const uint64_t t0 = __rdtsc();
        const bool result = call();
        stats.cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        return result;
    };
    const DWORD tid = GetCurrentThreadId();
    DWORD owner = g_thread.load(std::memory_order_relaxed);
    if (owner == 0 && g_thread.compare_exchange_strong(owner, tid)) owner = tid;
    unsigned long last_input = 0;
    POINT cursor = {};
    uint64_t now = 0;
    if (owner != tid || !g_sample(&last_input, &cursor.x, &cursor.y, &now)) {
        g_bypassed.fetch_add(1, std::memory_order_relaxed);
        return ask();
    }
    const uint64_t generation = Generation();
    const void* object_class = object ? *(void* const*)object : nullptr;
    size_t before[kStrings];
    for (size_t i = 0; i < kStrings; ++i) before[i] = TextOf(tip, i)->text.size();
    const bool same = e.valid && e.provider == provider && e.function == function && e.object == object &&
                      e.object_class == object_class && e.generation == generation && e.input == last_input &&
                      e.cursor.x == cursor.x && e.cursor.y == cursor.y && memcmp(e.before, before, sizeof before) == 0;
    const bool settled = (DWORD)((DWORD)now - last_input) >= kSettleMs;
    if (same && settled && now - e.built < (uint64_t)g_max_age_ms.load(std::memory_order_relaxed)) {
        if (mode != 2) {
            Restore(tip, e.snap);
            stats.kept.fetch_add(1, std::memory_order_relaxed);
            return e.snap.result;
        }
        const bool result = ask();
        if (Same(tip, result, e.snap)) {
            stats.kept.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_mismatches.fetch_add(1, std::memory_order_relaxed);
            Report(site, tip, result, e.snap, now - e.built);
            Capture(tip, result, &e.snap);  // one change counts once
            e.built = now;
        }
        return result;
    }
    const bool result = ask();
    stats.built.fetch_add(1, std::memory_order_relaxed);
    e.valid = settled;  // an answer built right after an input may predate the input's effect: it is not kept
    if (!settled) return result;
    Capture(tip, result, &e.snap);
    e.provider = provider;
    e.function = function;
    e.object = object;
    e.object_class = object_class;
    memcpy(e.before, before, sizeof before);
    e.generation = generation;
    e.input = last_input;
    e.cursor = cursor;
    e.built = now;
    return result;
}

// The stand-in handler: the manager calls its slot 0 right after the getter returned it.
bool StandInGetToolTip(void* self, void* object, void* tip);
struct StandIn {
    void* const* vtable;
};
void* const g_stand_in_vtable[1] = { (void*)&StandInGetToolTip };
StandIn g_stand_in = { g_stand_in_vtable };
thread_local void* t_handler = nullptr;  // the object's own handler, from the getter to that one call

// In place of the handler getter's call (rcx = the hovered object).
void* HandlerSite(void* object) {
    void* handler = (*(FnGetHandler* const*)object)[kHandlerSlot](object);
    if (!handler || g_mode.load(std::memory_order_relaxed) == 0) return handler;
    t_handler = handler;
    return &g_stand_in;
}

bool StandInGetToolTip(void*, void* object, void* tip) {
    void* handler = t_handler;
    const FnProvide get = (*(FnProvide* const*)handler)[0];
    return Answer(g_handler_entry, g_handler_stats, "handler", handler, (const void*)get, object, tip,
                  [&] { return get(handler, object, tip); });
}

// In place of the idler's BuildToolTip call (rcx = the idler, rdx = the hovered object or null, r8 = the tooltip).
bool IdlerSite(void* idler, void* object, void* tip) {
    const FnProvide build = (*(FnProvide* const*)idler)[kBuildSlot];
    return Answer(g_idler_entry, g_idler_stats, "idler", idler, (const void*)build, object, tip,
                  [&] { return build(idler, object, tip); });
}

// `push [the instruction after the call]; jmp [the function]`: the function runs as if the call instruction had
// called it (same stack, same return address, so the engine's unwind data stays right), wherever this DLL is loaded.
void WriteThunk(uint8_t* at, const void* function, const uint8_t* back) {
    static const uint8_t code[12] = { 0xFF, 0x35, 0x06, 0x00, 0x00, 0x00,    // push qword ptr [rip + 6]
                                      0xFF, 0x25, 0x08, 0x00, 0x00, 0x00 };  // jmp  qword ptr [rip + 8]
    memcpy(at, code, sizeof code);
    memcpy(at + 12, &back, sizeof back);
    memcpy(at + 20, &function, sizeof function);
}
constexpr size_t kThunkSize = 32;

bool SiteIsVirtualCall(const uint8_t* site, size_t slot) {
    const uint32_t disp = (uint32_t)(slot * sizeof(void*));
    __try {
        return site[0] == 0xFF && site[1] == 0x90 && memcmp(site + 2, &disp, sizeof disp) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The thunk returns to an address no call pushed: with hardware shadow stacks on for this process that would fault.
// (The 4.5 executables are not built for them.) PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY: bit 0 = enabled.
bool ShadowStacks() {
    DWORD flags = 0;
    return GetProcessMitigationPolicy(GetCurrentProcess(), (PROCESS_MITIGATION_POLICY)15, &flags, sizeof flags) && (flags & 1);
}

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
    return detail::InstallAt((uint8_t*)(base + kHandlerSite), (uint8_t*)(base + kIdlerSite), (void*)(base + sdk::fn::CString_Assign));
}

void detail::SetSampler(Sampler sample) { g_sample = sample ? sample : &SampleInput; }

bool detail::InstallAt(uint8_t* handler_site, uint8_t* idler_site, void* assign) {
    g_assign = (FnAssign)assign;
    if (ShadowStacks()) {
        Log("tooltip: this process runs with hardware shadow stacks; the cache stays off");
        return false;
    }
    if (!SiteIsVirtualCall(handler_site, kHandlerSlot) || !SiteIsVirtualCall(idler_site, kBuildSlot)) {
        Log("tooltip: CTooltipManager::Update's provider calls are not `call [rax + slot*8]` here; the cache stays off");
        return false;
    }
    // The thunks stay allocated for the life of the process: a thread may be about to run one when the hooks go.
    uint8_t* thunks = (uint8_t*)VirtualAlloc(nullptr, 2 * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!thunks) return false;
    WriteThunk(thunks, (const void*)&HandlerSite, handler_site + kSiteSize);
    WriteThunk(thunks + kThunkSize, (const void*)&IdlerSite, idler_site + kSiteSize);
    DWORD old;
    VirtualProtect(thunks, 2 * kThunkSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), thunks, 2 * kThunkSize);
    // MinHook replaces the first five bytes of each call by a jump (through its relay near the code) to the thunk; all
    // other threads are stopped while it writes, and MH_DisableHook puts the bytes back the same way.
    const bool created = MH_CreateHook(handler_site, thunks, nullptr) == MH_OK && MH_CreateHook(idler_site, thunks + kThunkSize, nullptr) == MH_OK;
    if (!created || MH_EnableHook(handler_site) != MH_OK || MH_EnableHook(idler_site) != MH_OK) {
        MH_RemoveHook(handler_site);
        MH_RemoveHook(idler_site);
        Log("tooltip: could not hook the provider calls; the cache stays off");
        return false;
    }
    g_installed = true;
    Log("tooltip: provider calls hooked at %p (the object's handler) and %p (the idler)", (void*)handler_site, (void*)idler_site);
    return true;
}

void SetMode(int mode) { g_mode.store(g_installed && mode >= 0 && mode <= 2 ? mode : 0); }
void SetMaxAgeMs(int ms) { g_max_age_ms.store(ms < 0 ? 0 : (ms > 5000 ? 5000 : ms)); }
bool Installed() { return g_installed; }

Counters Read() {
    return { g_handler_stats.kept.load() + g_idler_stats.kept.load(), g_handler_stats.built.load() + g_idler_stats.built.load(),
             g_bypassed.load(), g_mismatches.load(), g_handler_stats.cycles.load() + g_idler_stats.cycles.load() };
}

std::string Stats(double ms_per_cycle) {
    if (!g_installed) return "tooltip_cache=unavailable";
    const int mode = g_mode.load();
    auto site = [&](const char* name, const SiteStats& s) {
        const uint64_t kept = s.kept.load(), built = s.built.load();
        char buf[160];
        int n = snprintf(buf, sizeof buf, " %s kept=%llu built=%llu", name, (unsigned long long)kept, (unsigned long long)built);
        // verify asks the provider on kept frames too
        const uint64_t asked = built + (mode == 2 ? kept : 0);
        if (ms_per_cycle > 0 && asked) snprintf(buf + n, sizeof buf - n, " (%.3f ms each)", s.cycles.load() * ms_per_cycle / (double)asked);
        return std::string(buf);
    };
    std::string out = "tooltip_cache=" + std::to_string(mode) + site("handler", g_handler_stats) + site("idler", g_idler_stats) +
                      " bypassed=" + std::to_string(g_bypassed.load());
    if (mode == 2 || g_mismatches.load()) out += " mismatches=" + std::to_string(g_mismatches.load());
    return out;
}

}  // namespace perf::tooltip
