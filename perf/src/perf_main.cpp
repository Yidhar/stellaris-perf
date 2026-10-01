// stellaris_perf.dll entry: installs the perf hooks and follows stellaris_perf.ini (next to
// stellaris.exe) for the settings, re-reading it every 2 seconds so tweaks can be switched while
// the game runs. Statistics go to stellaris_perf.log every 30 seconds and on every change.
//
// Unloading: never FreeLibrary this DLL from outside while the game runs; a game thread may be inside
// a detour. Signal the event Local\stellaris_perf_unload_<pid> instead (scripts/reload_perf_dll.py):
// the worker removes the hooks, waits for in-flight calls to drain, then unloads the DLL itself.
#include "perf_tweaks.hpp"

#include <windows.h>
#include <atomic>
#include <string>

namespace {

HMODULE g_module = nullptr;
HANDLE g_unload_event = nullptr;

std::string IniPath() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/") + 1) + "stellaris_perf.ini";
}

void WriteDefaultIni(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE* f = fopen(path.c_str(), "w")) {
        fputs("; stellaris_perf.dll settings, re-read every 2 seconds while the game runs.\n"
              "; Defaults: the measured, result-identical fixes are on; the rest is off.\n"
              "[perf]\n"
              "; -1 = leave the game's setting alone, 0 = off (faster ticks), 1 = on (smoother UI)\n"
              "frame_smoothing=-1\n"
              "; fleet parallel update: one fleet per work chunk, so a doomstack does not stall the other\n"
              "; threads (same results)\n"
              "fleet_parallel_grain1=1\n"
              "; fleet manager window: cache fleet military power for up to 1 s (display only)\n"
              "fleet_manager_cache=1\n"
              "; fleet manager window: recalculate ships to reinforce at most every N ms (the game: 80 ms;\n"
              "; display only)\n"
              "fleet_manager_reinforce_ms=1000\n"
              "; has_*_flag scan: 0 = the game's, 1 = SSE2 (same answer), 2 = verify (count mismatches)\n"
              "flag_simd=0\n"
              "; 1 = skip the daily flag expiry pass on containers without timed flags (same results)\n"
              "flag_expiry_skip=0\n"
              "; approximations (not the game's exact behaviour), off: 1 = cache per game day\n"
              "opinion_cache=0\n"
              "; 0 = off, 1 = every rule, 2 = adaptive (only rules measured to gain) - per game day\n"
              "rule_cache=0\n"
              "; experimental, off: modifier flush fast path (0/1; 2 = verify, 3 = control)\n"
              "modifier_flush=0\n"
              "; multiplayer is lock-step: 1 = in a multiplayer session force off everything that touches the\n"
              "; simulation (the caches, the flag scans, the modifier fast path, the fleet chunking);\n"
              "; 0 = never override; 2 = act as if in multiplayer (to test the guard)\n"
              "multiplayer_guard=1\n", f);
        fclose(f);
    }
}

perf::Settings ReadIni(const std::string& path) {
    perf::Settings s;
    s.frame_smoothing = GetPrivateProfileIntA("perf", "frame_smoothing", -1, path.c_str());
    s.opinion_cache = GetPrivateProfileIntA("perf", "opinion_cache", 0, path.c_str()) != 0;
    s.rule_cache = GetPrivateProfileIntA("perf", "rule_cache", 0, path.c_str());
    s.fleet_manager_cache = GetPrivateProfileIntA("perf", "fleet_manager_cache", 1, path.c_str()) != 0;
    s.fleet_manager_reinforce_ms = GetPrivateProfileIntA("perf", "fleet_manager_reinforce_ms", 1000, path.c_str());
    s.flag_simd = GetPrivateProfileIntA("perf", "flag_simd", 0, path.c_str());
    s.flag_expiry_skip = GetPrivateProfileIntA("perf", "flag_expiry_skip", 0, path.c_str()) != 0;
    s.profile = GetPrivateProfileIntA("perf", "profile", 0, path.c_str()) != 0;
    s.rule_profile = GetPrivateProfileIntA("perf", "rule_profile", 0, path.c_str()) != 0;
    s.modifier_flush = GetPrivateProfileIntA("perf", "modifier_flush", 0, path.c_str());
    s.fleet_parallel_grain1 = GetPrivateProfileIntA("perf", "fleet_parallel_grain1", 1, path.c_str()) != 0;
    s.scope_profile = GetPrivateProfileIntA("perf", "scope_profile", 0, path.c_str()) != 0;
    s.modifier_flush_max = GetPrivateProfileIntA("perf", "modifier_flush_max", 32, path.c_str());
    s.multiplayer_guard = GetPrivateProfileIntA("perf", "multiplayer_guard", 1, path.c_str());
    return s;
}

DWORD WINAPI Worker(LPVOID) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    perf::Log("stellaris_perf.dll loaded, image base 0x%llX", (unsigned long long)base);
    if (!perf::Install(base)) {
        perf::Log("hooks not installed; the DLL stays idle");
        WaitForSingleObject(g_unload_event, INFINITE);
        perf::Uninstall();
        perf::Log("stellaris_perf.dll unloading");
        CloseHandle(g_unload_event);
        FreeLibraryAndExitThread(g_module, 0);
    }
    const std::string ini = IniPath();
    WriteDefaultIni(ini);
    // The multiplayer flag is checked every 0.5 s, the ini and the statistics every 2 s.
    perf::Settings user, last;
    bool first = true, last_mp = false, last_detected = false;
    int ticks = 0;
    for (;;) {
        if (first || ticks % 4 == 0) user = ReadIni(ini);
        const bool detected = perf::IsMultiplayerSession();
        const bool mp = user.multiplayer_guard == 2 || (user.multiplayer_guard == 1 && detected);
        const perf::Settings s = mp ? perf::ForMultiplayer(user) : user;
        const bool changed = first || s != last || mp != last_mp;
        if (first || detected != last_detected || mp != last_mp) {
            if (mp) {
                perf::Log("%s: settings that touch the simulation are forced off (opinion_cache, rule_cache, "
                          "modifier_flush, flag_simd, flag_expiry_skip, fleet_parallel_grain1)",
                          detected ? "multiplayer session detected" : "multiplayer_guard=2 (test)");
            } else if (detected) {
                perf::Log("multiplayer session detected but multiplayer_guard=0: the settings are NOT overridden; "
                          "every client must use identical settings or the game goes out of sync");
            } else if (!first) {
                perf::Log("single-player: the settings from the ini apply again");
            }
        }
        if (changed) {
            perf::Apply(s);
            perf::Log("settings: frame_smoothing=%d opinion_cache=%d rule_cache=%d fleet_manager_cache=%d "
                      "fleet_manager_reinforce_ms=%d flag_simd=%d flag_expiry_skip=%d fleet_parallel_grain1=%d "
                      "modifier_flush=%d profile=%d multiplayer=%d",
                      s.frame_smoothing, (int)s.opinion_cache, (int)s.rule_cache, (int)s.fleet_manager_cache,
                      s.fleet_manager_reinforce_ms, s.flag_simd, (int)s.flag_expiry_skip,
                      (int)s.fleet_parallel_grain1, s.modifier_flush, (int)s.profile, (int)mp);
            last = s;
            first = false;
        }
        last_mp = mp;
        last_detected = detected;
        if (ticks % 4 == 0) {
            perf::Publish();
            if (changed || ticks % 60 == 0) {
                perf::Log("%s", perf::StatsLine().c_str());
            }
        }
        ++ticks;
        if (WaitForSingleObject(g_unload_event, 500) == WAIT_OBJECT_0) break;
    }
    perf::Log("unload requested: %s", perf::StatsLine().c_str());
    perf::Uninstall();
    perf::Log("stellaris_perf.dll unloading");
    CloseHandle(g_unload_event);
    FreeLibraryAndExitThread(g_module, 0);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        g_module = module;
        char name[64];
        wsprintfA(name, "Local\\stellaris_perf_unload_%lu", GetCurrentProcessId());
        g_unload_event = CreateEventA(nullptr, TRUE, FALSE, name);
        if (!g_unload_event) return FALSE;
        // The worker releases the injector's reference with FreeLibraryAndExitThread.
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
        perf::Log("stellaris_perf.dll unloaded");
        break;
    }
    return TRUE;
}
