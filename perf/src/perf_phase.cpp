#include "perf_phase.hpp"
#include "perf_shared.hpp"
#include "perf_tweaks.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace perf::phase {

const char* const kNames[kCount] = { "tick", "micro", "daily", "monthly", "yearly", "ai", "save", "render_in_tick",
                                     "render_between", "frame", "gui", "gui2", "app", "draw" };

namespace {

// The date is stored in hours; the engine's CStellarisDate converts with this origin, 24 hours a day, 30-day months,
// 360-day years (so the 1st of a month is day-of-year % 30 == 0).
constexpr int32_t kHoursOrigin = 43800000;
constexpr int kMaxDays = 4096;
constexpr int kAll = kCount + kExtra;

struct DayRec {
    uint32_t day;        // days since the engine's origin
    uint16_t doy;        // day of year, 0 = 1 January
    uint16_t ticks;      // ticks attributed to the day (10 when complete)
    uint64_t wall;       // cycles from the start of the day's first tick to the start of the next day's first tick
    uint64_t ph[kAll];   // exclusive cycles per phase
    uint32_t calls[kAll];  // phase entries (render_in_tick / render_between: frames drawn; gui: frame function iterations)
};

std::atomic<bool> g_on{ false };
std::atomic<bool> g_installed{ false };
double g_tsc_per_sec = 0;
uintptr_t g_base = 0;

// ---- main-thread state (the first HandleTurnTick names the thread) ----
DWORD g_tid = 0;
bool g_started = false;        // accumulators were reset after the setting came on
struct Frame {
    int p;
    uint64_t t0;
    uint64_t child;
};
Frame g_stack[48];
int g_depth = 0;
int g_in_render = 0;
int g_in_tick = 0;
uint64_t g_acc[kAll];        // the current day, phases between ticks
uint64_t g_tick_acc[kAll];   // the tick in progress (attributed to a day when it ends)
uint32_t g_calls[kAll], g_tick_calls[kAll];
uint64_t g_tick_t0 = 0;
uint32_t g_cur_day = 0xFFFFFFFF;
uint16_t g_cur_doy = 0, g_cur_ticks = 0;
uint64_t g_day_t0 = 0;

// ---- day records: written by the main thread, read by the worker (count published with release/acquire) ----
DayRec g_days[kMaxDays];
std::atomic<uint32_t> g_ndays{ 0 };
std::atomic<uint32_t> g_dropped{ 0 };  // days not recorded because the table is full

// ---- extra entry points from the ini (phase_extra), hooked while the game runs ----
struct Extra {
    uintptr_t rva;
    char name[40];
    bool hooked;
    std::atomic<bool> active;  // named in the current phase_extra; a hook is never removed, only ignored
};
Extra g_extra[kExtra];
std::atomic<int> g_nextra{ 0 };

// ---- call-site histogram of one timed function (phase_callers): main thread writes, the worker reads a snapshot ----
struct Site {
    uintptr_t site;  // return address (RVA); 0 = empty
    uint32_t calls;
    uint64_t cycles;
};
constexpr int kSites = 512;
Site g_sites[2][kSites];  // [0] normal days, [1] first-of-month days
std::atomic<uint32_t> g_site_overflow{ 0 };
std::atomic<int> g_caller_slot{ -1 };  // phase index whose call sites are recorded, -1 = none

bool ReadDate(uint32_t* day, uint16_t* doy);

void RecordCaller(uintptr_t site, uint64_t dt) {
    // the date is read now, not taken from the day window: the window moves only when the tick that advanced the date
    // ends, while the daily and monthly updates of the new day run inside that tick
    uint32_t day = 0;
    uint16_t doy = g_cur_doy;
    ReadDate(&day, &doy);
    Site* tab = g_sites[doy % 30 == 0 ? 1 : 0];
    uint32_t h = (uint32_t)(((uint64_t)site * 0x9E3779B97F4A7C15ull) >> (64 - 9));
    for (int k = 0; k < 16; ++k) {
        Site& s = tab[(h + k) & (kSites - 1)];
        if (s.site == site || s.site == 0) {
            s.site = site;
            ++s.calls;
            s.cycles += dt;
            return;
        }
    }
    g_site_overflow.fetch_add(1, std::memory_order_relaxed);
}

bool ReadDate(uint32_t* day, uint16_t* doy) {
    __try {
        const uintptr_t gs = *(const uintptr_t*)(g_base + sdk::glob::g_CurrentGameState);
        if (!gs) return false;
        const int32_t hours = *(const int32_t*)(gs + sdk::rt::CGameState_date_hours);
        if (hours < kHoursOrigin) return false;
        const uint32_t d = (uint32_t)(hours - kHoursOrigin) / 24;
        *day = d;
        *doy = (uint16_t)(d % 360);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void StartDay(uint32_t day, uint16_t doy, uint64_t t0) {
    g_cur_day = day;
    g_cur_doy = doy;
    g_cur_ticks = 0;
    g_day_t0 = t0;
    memset(g_acc, 0, sizeof g_acc);
    memset(g_calls, 0, sizeof g_calls);
}

void FinishDay(uint64_t t_end) {
    const uint32_t n = g_ndays.load(std::memory_order_relaxed);
    if (n >= kMaxDays) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    DayRec& r = g_days[n];
    r.day = g_cur_day;
    r.doy = g_cur_doy;
    r.ticks = g_cur_ticks;
    r.wall = t_end - g_day_t0;
    memcpy(r.ph, g_acc, sizeof r.ph);
    memcpy(r.calls, g_calls, sizeof r.calls);
    g_ndays.store(n + 1, std::memory_order_release);
}

// ---- the hooks: 4.5.2 entry points, each checked against its first bytes ----
using Fn = uint64_t (*)(void*, void*, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
Fn g_orig[kAll];

// Forwards four register and four stack arguments, enough for every function here; no floating point is touched, so
// xmm arguments and results pass through untouched.
template <int P>
uint64_t Detour(void* a, void* b, void* c, void* d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    SharedStats* shared = nullptr;
    if constexpr (P == kSave) {  // the save's window, for a sampling profiler outside the game (perf_shared.hpp)
        if ((shared = Shared()) != nullptr) {
            shared->save_tid = GetCurrentThreadId();
            InterlockedExchange((volatile LONG*)&shared->save_active, 1);
        }
    }
    uint64_t r;
    if (P == g_caller_slot.load(std::memory_order_relaxed)) {
        // the hook jumps here from the function's entry, so the return address is the caller's call site
        const uintptr_t site = (uintptr_t)_ReturnAddress() - g_base;
        const bool opened = Enter(P);
        const uint64_t t0 = __rdtsc();
        r = g_orig[P](a, b, c, d, e, f, g, h);
        const uint64_t dt = __rdtsc() - t0;
        Exit(opened);
        if (opened) RecordCaller(site, dt);
    } else {
        const bool opened = Enter(P);
        r = g_orig[P](a, b, c, d, e, f, g, h);
        Exit(opened);
    }
    if constexpr (P == kSave) {
        if (shared) InterlockedExchange((volatile LONG*)&shared->save_active, 0);
    }
    return r;
}

template <size_t... I>
constexpr std::array<void*, sizeof...(I)> MakeExtraDetours(std::index_sequence<I...>) {
    return { (void*)&Detour<kCount + (int)I>... };
}
const std::array<void*, kExtra> kExtraDetours = MakeExtraDetours(std::make_index_sequence<kExtra>{});

struct Target {
    Phase phase;
    uintptr_t rva;
    const char* name;
    uint8_t head[8];
    void* detour;
};
const Target kTargets[] = {
    { kMicro, 0x252CC0, "CGameState::MicroUpdate", { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56 }, (void*)&Detour<kMicro> },
    { kDaily, 0x25BDB0, "CGameState::DailyUpdate", { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56 }, (void*)&Detour<kDaily> },
    { kMonthly, 0x2605F0, "CGameState::MonthlyUpdate", { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 }, (void*)&Detour<kMonthly> },
    { kYearly, 0x261EB0, "CGameState::YearlyUpdate", { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41 }, (void*)&Detour<kYearly> },
    { kAi, 0x2636C0, "CGameState::ProcessAI", { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56 }, (void*)&Detour<kAi> },
    // CInGameIdler::AutoSave only queues the save (CSaveGameTaskManager::Start); the game state is dumped by the next
    // frame's CSaveGameTaskManager::Update (called from the frame function through the idler, and from the OOS dump),
    // which runs the dump task on the main thread and draws frames while it waits for the rest.
    { kSave, 0xA89AD0, "CSaveGameTaskManager::Update", { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41 }, (void*)&Detour<kSave> },
    { kDraw, 0x334160, "frame draw step", { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 }, (void*)&Detour<kDraw> },
    { kGui, 0x9293A0, "in-game idler update", { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55 }, (void*)&Detour<kGui> },
    { kGui2, 0x1C0A7A0, "GUI update", { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x4C, 0x89, 0x44 }, (void*)&Detour<kGui2> },
    { kApp, 0x1A4D610, "application update", { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 }, (void*)&Detour<kApp> },
};

const char* NameOf(int p) { return p < kCount ? kNames[p] : g_extra[p - kCount].name; }

// True when rva is the start of a function of the exe: an entry of the exception directory whose unwind info is not a
// chained one (a chunk of another function). Hooking anything else would corrupt the code, so phase_extra is checked.
bool IsFunctionStart(uintptr_t rva) {
    __try {
        auto dos = (const IMAGE_DOS_HEADER*)g_base;
        auto nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir.VirtualAddress || !dir.Size) return false;
        auto rf = (const RUNTIME_FUNCTION*)(g_base + dir.VirtualAddress);
        size_t lo = 0, hi = dir.Size / sizeof(RUNTIME_FUNCTION);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (rf[mid].BeginAddress < rva) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= dir.Size / sizeof(RUNTIME_FUNCTION) || rf[lo].BeginAddress != rva) return false;
        const uint8_t flags = *(const uint8_t*)(g_base + rf[lo].UnwindData) >> 3;
        return (flags & UNW_FLAG_CHAININFO) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
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
    int hooked = 0;
    for (const Target& t : kTargets) {
        uint8_t* p = (uint8_t*)(base + t.rva);
        if (memcmp(p, t.head, sizeof t.head) != 0) {
            Log("phase: %s at RVA 0x%llX does not start as expected; not timed", t.name, (unsigned long long)t.rva);
            continue;
        }
        if (MH_CreateHook(p, t.detour, (LPVOID*)&g_orig[t.phase]) != MH_OK || MH_EnableHook(p) != MH_OK) {
            Log("phase: could not hook %s; not timed", t.name);
            g_orig[t.phase] = nullptr;
            continue;
        }
        ++hooked;
    }
    g_installed.store(hooked > 0);
    Log("phase: %d of %d entry points hooked", hooked, (int)(sizeof kTargets / sizeof kTargets[0]));
    return hooked > 0;
}

void SetEnabled(bool on) {
    if (on == g_on.load()) return;
    if (on) g_ndays.store(0);  // a fresh table; the main thread resets its accumulators on its next tick
    g_started = false;
    g_on.store(on);
}

bool Enabled() { return g_on.load(std::memory_order_relaxed); }

void SetExtra(const char* spec) {
    if (!g_installed.load() || !spec) return;
    const int n0 = g_nextra.load();
    bool wanted[kExtra] = {};
    // "0xRVA[=name], 0xRVA[=name], ..."
    const char* s = spec;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == ';') ++s;
        if (!*s) break;
        char* end = nullptr;
        const uintptr_t rva = (uintptr_t)strtoull(s, &end, 16);
        if (end == s) {  // not a number: skip the token
            while (*s && *s != ',' && *s != ';') ++s;
            continue;
        }
        s = end;
        char name[40] = {};
        if (*s == '=') {
            ++s;
            int k = 0;
            while (*s && *s != ',' && *s != ';' && *s != ' ') {
                if (k < (int)sizeof name - 1) name[k++] = *s;
                ++s;
            }
        }
        if (!name[0]) snprintf(name, sizeof name, "x_%llX", (unsigned long long)rva);
        int slot = -1;
        for (int i = 0; i < n0; ++i) {
            if (g_extra[i].rva == rva) { slot = i; break; }
        }
        if (slot < 0) {
            const int n = g_nextra.load();
            if (n >= kExtra) {
                Log("phase_extra: no slot left for 0x%llX (%d at most)", (unsigned long long)rva, kExtra);
                continue;
            }
            Extra& e = g_extra[n];
            e.rva = rva;
            memcpy(e.name, name, sizeof e.name);
            e.hooked = false;
            if (!IsFunctionStart(rva)) {
                Log("phase_extra: 0x%llX is not the start of a function of the exe; ignored", (unsigned long long)rva);
            } else if (MH_CreateHook((LPVOID)(g_base + rva), kExtraDetours[n], (LPVOID*)&g_orig[kCount + n]) != MH_OK ||
                       MH_EnableHook((LPVOID)(g_base + rva)) != MH_OK) {
                Log("phase_extra: could not hook 0x%llX (already hooked by another option?); ignored", (unsigned long long)rva);
                g_orig[kCount + n] = nullptr;
            } else {
                e.hooked = true;
                Log("phase_extra: timing 0x%llX as %s", (unsigned long long)rva, e.name);
            }
            g_nextra.store(n + 1, std::memory_order_release);
            slot = n;
        } else if (name[0] && strcmp(g_extra[slot].name, name) != 0) {
            memcpy(g_extra[slot].name, name, sizeof name);
        }
        if (slot < kExtra) wanted[slot] = g_extra[slot].hooked;
    }
    const int n = g_nextra.load();
    for (int i = 0; i < n; ++i) g_extra[i].active.store(wanted[i]);
}

void SetCallers(const char* spec) {
    if (!g_installed.load() || !spec) return;
    char* end = nullptr;
    const uintptr_t rva = (uintptr_t)strtoull(spec, &end, 16);
    int slot = -1;
    if (end != spec && rva) {
        for (const Target& t : kTargets) {
            if (t.rva == rva && g_orig[t.phase]) slot = t.phase;
        }
        const int n = g_nextra.load();
        for (int i = 0; i < n; ++i) {
            if (g_extra[i].rva == rva && g_extra[i].hooked) slot = kCount + i;
        }
        if (slot < 0) Log("phase_callers: 0x%llX is not a timed function (name it in phase_extra first); ignored", (unsigned long long)rva);
    }
    if (slot == g_caller_slot.load()) return;
    g_caller_slot.store(-1);
    Sleep(20);  // a call in flight finishes before the tables are cleared (the main thread writes them)
    memset(g_sites, 0, sizeof g_sites);
    g_site_overflow.store(0);
    g_caller_slot.store(slot);
    if (slot >= 0) Log("phase_callers: recording the call sites of %s", NameOf(slot));
}

bool Enter(int p) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    const DWORD tid = GetCurrentThreadId();
    if (p == kTick) {
        if (g_tid == 0) g_tid = tid;
        if (tid != g_tid) return false;
        if (!g_started) {  // the setting just came on: start clean
            g_depth = 0;
            g_in_render = 0;
            g_in_tick = 0;
            memset(g_acc, 0, sizeof g_acc);
            memset(g_calls, 0, sizeof g_calls);
            g_cur_day = 0xFFFFFFFF;
            g_started = true;
        }
        memset(g_tick_acc, 0, sizeof g_tick_acc);
        memset(g_tick_calls, 0, sizeof g_tick_calls);
        ++g_in_tick;
    } else {
        if (tid != g_tid || !g_started) return false;
        if (g_in_render > 0) return false;  // everything inside RenderFrame is rendering
        if (p >= kCount && !g_extra[p - kCount].active.load(std::memory_order_relaxed)) return false;
        if (p == kRenderTick || p == kRenderFrame) {
            p = g_in_tick > 0 ? kRenderTick : kRenderFrame;
            ++g_in_render;
        }
    }
    if (g_depth >= (int)(sizeof g_stack / sizeof g_stack[0])) {
        if (p == kRenderTick || p == kRenderFrame) --g_in_render;
        if (p == kTick) --g_in_tick;
        return false;
    }
    Frame& f = g_stack[g_depth++];
    f.p = p;
    f.t0 = __rdtsc();
    f.child = 0;
    if (p == kTick) g_tick_t0 = f.t0;
    ++(g_in_tick > 0 ? g_tick_calls : g_calls)[p];
    return true;
}

void Exit(bool opened) {
    if (!opened || g_depth <= 0) return;
    const uint64_t now = __rdtsc();
    Frame& f = g_stack[--g_depth];
    const uint64_t dt = now - f.t0;
    const uint64_t excl = dt > f.child ? dt - f.child : 0;
    (g_in_tick > 0 ? g_tick_acc : g_acc)[f.p] += excl;
    if (g_depth > 0) g_stack[g_depth - 1].child += dt;
    if (f.p == kRenderTick || f.p == kRenderFrame) --g_in_render;
    if (f.p == kTick) --g_in_tick;
}

void OnTickEnd() {
    if (!g_on.load(std::memory_order_relaxed) || !g_started || GetCurrentThreadId() != g_tid) return;
    uint32_t day;
    uint16_t doy;
    if (!ReadDate(&day, &doy)) return;
    if (day != g_cur_day) {
        // this tick moved the date: it belongs to the new day, from its start
        if (g_cur_day != 0xFFFFFFFF) FinishDay(g_tick_t0);
        StartDay(day, doy, g_tick_t0);
    }
    for (int i = 0; i < kAll; ++i) {
        g_acc[i] += g_tick_acc[i];
        g_calls[i] += g_tick_calls[i];
    }
    ++g_cur_ticks;
}

void WriteJson() {
    if (!g_installed.load()) return;
    const std::wstring dir = LogDirectory();
    if (dir.empty()) return;
    const uint32_t n = g_ndays.load(std::memory_order_acquire);
    const int cols = kCount + g_nextra.load(std::memory_order_acquire);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tmp = dir + L"\\stellaris_perf_phases.json.tmp";
    FILE* f = _wfopen(tmp.c_str(), L"w");
    if (!f) return;
    fprintf(f, "{\"tsc_per_sec\": %.0f, \"dropped\": %u, \"fixed\": %d, \"names\": [", g_tsc_per_sec, g_dropped.load(), (int)kCount);
    for (int i = 0; i < cols; ++i) fprintf(f, "%s\"%s\"", i ? ", " : "", NameOf(i));
    fprintf(f, "],\n\"days\": [\n");
    for (uint32_t i = 0; i < n; ++i) {
        const DayRec& r = g_days[i];
        fprintf(f, "%s[%u, %u, %u, %llu", i ? ",\n" : "", r.day, (unsigned)r.doy, (unsigned)r.ticks, (unsigned long long)r.wall);
        for (int k = 0; k < cols; ++k) fprintf(f, ", %llu", (unsigned long long)r.ph[k]);
        for (int k = 0; k < cols; ++k) fprintf(f, ", %u", r.calls[k]);
        fputs("]", f);
    }
    fputs("\n]", f);
    const int slot = g_caller_slot.load();
    if (slot >= 0) {
        fprintf(f, ",\n\"callers\": {\"of\": \"%s\", \"overflow\": %u", NameOf(slot), g_site_overflow.load());
        for (int t = 0; t < 2; ++t) {
            fprintf(f, ", \"%s\": [", t ? "month" : "normal");
            bool first = true;
            for (int i = 0; i < kSites; ++i) {
                const Site s = g_sites[t][i];  // a racy copy: a count may be one call behind, never garbage
                if (!s.site) continue;
                fprintf(f, "%s[%llu, %u, %llu]", first ? "" : ", ", (unsigned long long)s.site, s.calls, (unsigned long long)s.cycles);
                first = false;
            }
            fputs("]", f);
        }
        fputs("}", f);
    }
    fputs("}\n", f);
    fclose(f);
    MoveFileExW(tmp.c_str(), (dir + L"\\stellaris_perf_phases.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string Summary() {
    if (!g_installed.load()) return "phase_profile=unavailable";
    if (!g_on.load()) return "phase_profile=off";
    const uint32_t n = g_ndays.load(std::memory_order_acquire);
    // the last 200 complete days, normal days against first-of-month days
    double wall[2] = { 0, 0 }, tick[2] = { 0, 0 }, render[2] = { 0, 0 }, ui[2] = { 0, 0 };
    int cnt[2] = { 0, 0 };
    for (uint32_t i = n > 200 ? n - 200 : 0; i < n; ++i) {
        const DayRec& r = g_days[i];
        if (r.ticks != 10) continue;
        const int b = r.doy % 30 == 0 ? 1 : 0;
        ++cnt[b];
        wall[b] += (double)r.wall;
        tick[b] += (double)(r.ph[kTick] + r.ph[kMicro] + r.ph[kDaily] + r.ph[kMonthly] + r.ph[kYearly] + r.ph[kAi] + r.ph[kSave]);
        render[b] += (double)(r.ph[kRenderTick] + r.ph[kRenderFrame]);
        ui[b] += (double)(r.ph[kFrame] + r.ph[kGui] + r.ph[kGui2] + r.ph[kApp] + r.ph[kDraw]);
    }
    const double ms = 1000.0 / g_tsc_per_sec;
    char buf[320];
    auto part = [&](int b) {
        char s[140];
        if (!cnt[b]) return std::string("none yet");
        const double k = ms / cnt[b];
        snprintf(s, sizeof s, "%d days wall %.1f ms (tick %.1f, render %.1f, ui %.1f)", cnt[b], wall[b] * k, tick[b] * k, render[b] * k, ui[b] * k);
        return std::string(s);
    };
    snprintf(buf, sizeof buf, "phase_profile=on days=%u extra=%d normal: %s | month start: %s", n, g_nextra.load(), part(0).c_str(), part(1).c_str());
    return std::string(buf);
}

}  // namespace perf::phase
