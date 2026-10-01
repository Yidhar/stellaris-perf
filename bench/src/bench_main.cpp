// stellaris_bench.dll entry: patches the Present slot, then serves one-line commands on
// \\.\pipe\stellaris_bench (one client at a time, one reply line per command line).
//
// Unloading: never FreeLibrary this DLL from outside; set the event
// Local\stellaris_bench_unload_<pid> (stellaris_bench/scripts/dllctl.py). The worker restores the
// Present slot, stops the pipe thread, waits for in-flight frames and unloads the DLL itself.
#include "bench.hpp"

#include <windows.h>
#include <atomic>
#include <string>

namespace {

HMODULE g_module = nullptr;
HANDLE g_unload_event = nullptr;
HANDLE g_pipe_thread = nullptr;
std::atomic<bool> g_stopping{ false };

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\stellaris_bench";
constexpr unsigned kCommandTimeoutMs = 3000;

void Serve(HANDLE pipe) {
    std::string pending;
    char buf[512];
    while (!g_stopping) {
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &got, nullptr) || got == 0) return;
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const std::string reply = bench::Execute(line, kCommandTimeoutMs) + "\n";
            DWORD put = 0;
            if (!WriteFile(pipe, reply.data(), (DWORD)reply.size(), &put, nullptr)) return;
        }
    }
}

DWORD WINAPI PipeThread(LPVOID) {
    while (!g_stopping) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            bench::Log("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && !g_stopping) Serve(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

void StopPipeThread() {
    g_stopping = true;
    if (!g_pipe_thread) return;
    // ConnectNamedPipe / ReadFile block synchronously: cancel them
    for (int i = 0; i < 50 && WaitForSingleObject(g_pipe_thread, 0) == WAIT_TIMEOUT; ++i) {
        CancelSynchronousIo(g_pipe_thread);
        Sleep(20);
    }
    if (WaitForSingleObject(g_pipe_thread, 2000) == WAIT_TIMEOUT) bench::Log("pipe thread did not stop");
    CloseHandle(g_pipe_thread);
    g_pipe_thread = nullptr;
}

DWORD WINAPI Worker(LPVOID) {
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    bench::Log("stellaris_bench.dll loaded, image base 0x%llX", (unsigned long long)base);
    if (bench::Install(base)) {
        g_pipe_thread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    } else {
        bench::Log("not installed; the DLL stays idle");
    }
    WaitForSingleObject(g_unload_event, INFINITE);
    bench::Log("unload requested");
    StopPipeThread();
    bench::Uninstall();
    bench::Log("stellaris_bench.dll unloading");
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
        wsprintfA(name, "Local\\stellaris_bench_unload_%lu", GetCurrentProcessId());
        g_unload_event = CreateEventA(nullptr, TRUE, FALSE, name);
        if (!g_unload_event) return FALSE;
        // The worker releases the injector's reference with FreeLibraryAndExitThread.
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
        bench::Log("stellaris_bench.dll unloaded");
        break;
    }
    return TRUE;
}
