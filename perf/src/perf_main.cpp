// stellaris_perf.dll entry: a stellaris-launcher plugin (plugin spec v2, docs/PLUGINS.md of the launcher).
//
// The plugin lives in one folder, <Documents>\Paradox Interactive\Stellaris\plugins\stellaris-perf\, which the DLL finds
// from its own module (not from the game or working folder):
//   config\stellaris_perf.ini   the settings; re-read while the game runs, whenever the file's modification time changes
//   logs\stellaris_perf.log     statistics every 30 seconds and on every change; logs\stellaris_perf_rules.csv (rule_profile)
// A missing settings file means the built-in defaults. Nothing is written into the game folder. An old
// stellaris_perf.ini next to stellaris.exe is copied into config\ once, when config\ has none yet, and ignored after that.
//
// DllMain only makes the unload event and starts the worker thread; everything else happens on the worker.
//
// Unloading: never FreeLibrary this DLL from outside while the game runs; a game thread may be inside a detour. Signal the
// event Local\stellaris_perf_unload_<pid> instead (bench/scripts/dllctl.py unload perf): the worker removes the
// hooks, waits for in-flight calls to drain and unloads the DLL itself.
#include "perf_tweaks.hpp"

#include <windows.h>
#include <atomic>
#include <string>

namespace {

HMODULE g_module = nullptr;
HANDLE g_unload_event = nullptr;

// The folder this DLL was loaded from.
std::wstring PluginDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&PluginDir, &self)) {
        return L"";
    }
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(self, path.data(), (DWORD)path.size());
    if (n == 0 || n >= path.size()) return L"";
    path.resize(n);
    return path.substr(0, path.find_last_of(L"\\/"));
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// The old place of the settings: next to stellaris.exe. Copied once when config\ has no file; never read afterwards.
void MigrateGameFolderIni(const std::wstring& config_dir, const std::wstring& config_file) {
    if (GetFileAttributesW(config_file.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (n == 0 || n >= exe.size()) return;
    exe.resize(n);
    const std::wstring old_ini = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"stellaris_perf.ini";
    if (GetFileAttributesW(old_ini.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryW(config_dir.c_str(), nullptr);
    if (CopyFileW(old_ini.c_str(), config_file.c_str(), TRUE)) {
        perf::Log("settings: copied %s from the game folder to %s (the game folder copy is ignored from now on)",
                  Utf8(old_ini).c_str(), Utf8(config_file).c_str());
    } else {
        perf::Log("settings: could not copy %s (error %lu)", Utf8(old_ini).c_str(), GetLastError());
    }
}

// What identifies one version of the settings file.
struct FileStamp {
    bool exists = false;
    FILETIME written{};
    DWORD size = 0;
    bool operator==(const FileStamp& o) const {
        return exists == o.exists && written.dwLowDateTime == o.written.dwLowDateTime &&
               written.dwHighDateTime == o.written.dwHighDateTime && size == o.size;
    }
};

FileStamp StampOf(const std::wstring& path) {
    FileStamp s;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
        s.exists = true;
        s.written = d.ftLastWriteTime;
        s.size = d.nFileSizeLow;
    }
    return s;
}

// Every key has its built-in default here, so a missing file or a missing key just means the default.
perf::Settings ReadIni(const std::wstring& path) {
    perf::Settings s;
    const wchar_t* p = path.c_str();
    s.frame_smoothing = GetPrivateProfileIntW(L"perf", L"frame_smoothing", -1, p);
    s.opinion_cache = GetPrivateProfileIntW(L"perf", L"opinion_cache", 0, p) != 0;
    s.rule_cache = GetPrivateProfileIntW(L"perf", L"rule_cache", 0, p);
    s.fleet_manager_cache = GetPrivateProfileIntW(L"perf", L"fleet_manager_cache", 1, p) != 0;
    s.fleet_manager_reinforce_ms = GetPrivateProfileIntW(L"perf", L"fleet_manager_reinforce_ms", 1000, p);
    s.flag_simd = GetPrivateProfileIntW(L"perf", L"flag_simd", 0, p);
    s.flag_expiry_skip = GetPrivateProfileIntW(L"perf", L"flag_expiry_skip", 0, p) != 0;
    s.profile = GetPrivateProfileIntW(L"perf", L"profile", 0, p) != 0;
    s.rule_profile = GetPrivateProfileIntW(L"perf", L"rule_profile", 0, p) != 0;
    s.modifier_flush = GetPrivateProfileIntW(L"perf", L"modifier_flush", 0, p);
    s.fleet_parallel_grain1 = GetPrivateProfileIntW(L"perf", L"fleet_parallel_grain1", 1, p) != 0;
    s.scope_profile = GetPrivateProfileIntW(L"perf", L"scope_profile", 0, p) != 0;
    s.modifier_flush_max = GetPrivateProfileIntW(L"perf", L"modifier_flush_max", 32, p);
    s.multiplayer_guard = GetPrivateProfileIntW(L"perf", L"multiplayer_guard", 1, p);
    return s;
}

[[noreturn]] void Unload() {
    perf::Uninstall();
    perf::Log("stellaris_perf.dll unloading");
    CloseHandle(g_unload_event);
    FreeLibraryAndExitThread(g_module, 0);
}

DWORD WINAPI Worker(LPVOID) {
    const std::wstring dir = PluginDir();
    perf::SetLogDirectory(dir.empty() ? L"" : dir + L"\\logs");
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    perf::Log("stellaris_perf.dll loaded, image base 0x%llX, plugin folder %s", (unsigned long long)base,
              dir.empty() ? "(unknown: no log file, built-in defaults)" : Utf8(dir).c_str());
    if (!perf::Install(base)) {
        perf::Log("hooks not installed; the DLL stays idle");
        WaitForSingleObject(g_unload_event, INFINITE);
        Unload();
    }
    const std::wstring config_dir = dir.empty() ? L"" : dir + L"\\config";
    const std::wstring config_file = dir.empty() ? L"" : config_dir + L"\\stellaris_perf.ini";
    if (!dir.empty()) MigrateGameFolderIni(config_dir, config_file);

    // A game that is already running when the DLL is loaded started before the hooks existed: read its
    // multiplayer flag now. Later games are checked by the hooks, when they start.
    perf::CheckMultiplayer("DLL loaded");

    perf::Settings settings;  // the built-in defaults until a file is read
    FileStamp stamp;
    bool first = true;
    int ticks = 0;
    for (;;) {
        // The settings are read when the file changed (also: appeared, or went away), checked every 2 s.
        const FileStamp now = config_file.empty() ? FileStamp{} : StampOf(config_file);
        if (first || !(now == stamp)) {
            if (now.exists) {
                settings = ReadIni(config_file);
                perf::Log("settings file %s read", Utf8(config_file).c_str());
            } else {
                settings = perf::Settings{};
                perf::Log("no settings file%s%s: the built-in defaults apply", config_file.empty() ? "" : " at ",
                          Utf8(config_file).c_str());
            }
            stamp = now;
            first = false;
        }
        const bool changed = perf::SetUserSettings(settings);
        perf::Publish();
        if (changed || ++ticks % 15 == 0) {
            perf::Log("%s", perf::StatsLine().c_str());
        }
        if (WaitForSingleObject(g_unload_event, 2000) == WAIT_OBJECT_0) break;
    }
    perf::Log("unload requested: %s", perf::StatsLine().c_str());
    Unload();
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
