#include "bench.hpp"
#include "stellaris_sdk.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

namespace bench {

// ---- log ----------------------------------------------------------------------------------

namespace {
std::mutex g_log_mutex;
FILE* g_log = nullptr;
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "stellaris_bench.log");
        g_log = fopen(path, "a");
        if (!g_log) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

namespace {

using FnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags);
using FnSetPaused = void (*)(void* idler, const void* settings);
using FnSetGameSpeed = void (*)(void* idler, int speed);

uintptr_t g_base = 0;
void** g_present_slot = nullptr;  // IDXGISwapChain::Present in DXGI's swap chain vtable
FnPresent g_orig_present = nullptr;
std::atomic<int> g_in_present{ 0 };
std::atomic<uint64_t> g_frames{ 0 };
double g_qpc_period = 0;

// ---- main-thread commands -----------------------------------------------------------------

struct Task {
    std::string line;
    std::string reply;
    bool done = false;
};
std::mutex g_tasks_mutex;
std::condition_variable g_tasks_cv;
std::deque<std::shared_ptr<Task>> g_tasks;

double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * g_qpc_period;
}

void* ReadPtr(uintptr_t addr) {
    __try {
        return *(void* const*)addr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadU32(uintptr_t addr, uint32_t* out) {
    __try {
        *out = *(const uint32_t*)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* Idler() { return ReadPtr(g_base + sdk::glob::g_CurrentInGameIdler); }
void* GameState() { return ReadPtr(g_base + sdk::glob::g_CurrentGameState); }

std::string Status() {
    void* idler = Idler();
    void* state = GameState();
    uint32_t hours = 0, speed = 0, paused_word = 0;
    const bool in_game = idler && state && ReadU32((uintptr_t)state + sdk::rt::CGameState_date_hours, &hours) &&
                         ReadU32((uintptr_t)idler + sdk::rt::CInGameIdler_speed, &speed) &&
                         ReadU32((uintptr_t)idler + sdk::rt::CInGameIdler_paused, &paused_word);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"in_game\":%s,\"hours\":%u,\"day\":%u,\"paused\":%s,\"speed\":%u,\"frames\":%llu,\"t\":%.6f}",
             in_game ? "true" : "false", hours, hours / 24, (paused_word & 0xFF) ? "true" : "false", speed,
             (unsigned long long)g_frames.load(), Now());
    return buf;
}

// SPauseGameSettings (Windows): +0x10 std::string naming who pauses (empty), +0x30 bool paused,
// +0x31 source; 2 goes past the lock a named pauser holds (as the UI's own button does).
struct PauseSettings {
    uint64_t unknown[2]{};
    char who[16]{};
    uint64_t who_size = 0;
    uint64_t who_capacity = 15;
    uint8_t paused = 1;
    uint8_t source = 2;
    uint8_t pad[14]{};
};
static_assert(offsetof(PauseSettings, paused) == 0x30, "SPauseGameSettings layout");

bool CallSetPaused(void* idler, bool paused) {
    PauseSettings settings;
    settings.paused = paused ? 1 : 0;
    __try {
        ((FnSetPaused)(g_base + sdk::fn::CInGameIdler_SetPaused))(idler, &settings);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetGameSpeed(void* idler, int speed) {
    __try {
        ((FnSetGameSpeed)(g_base + sdk::fn::CInGameIdler_SetGameSpeed))(idler, speed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string Error(const char* what) { return std::string("{\"ok\":false,\"error\":\"") + what + "\"}"; }

std::string Run(const std::string& line) {
    char cmd[32] = {};
    int arg = 0;
    const int n = sscanf(line.c_str(), "%31s %d", cmd, &arg);
    if (n < 1) return Error("empty command");
    if (!strcmp(cmd, "status")) return Status();
    if (!strcmp(cmd, "pause") || !strcmp(cmd, "speed")) {
        if (n < 2) return Error("missing argument");
        void* idler = Idler();
        if (!idler || !GameState()) return Error("not in game");
        const bool ok = !strcmp(cmd, "pause") ? CallSetPaused(idler, arg != 0) : CallSetGameSpeed(idler, arg);
        if (!ok) return Error("exception in the engine call");
        return Status();
    }
    return Error("unknown command");
}

void DrainTasks() {
    std::unique_lock<std::mutex> lock(g_tasks_mutex);
    while (!g_tasks.empty()) {
        std::shared_ptr<Task> task = g_tasks.front();
        g_tasks.pop_front();
        lock.unlock();
        std::string reply = Run(task->line);
        lock.lock();
        task->reply = std::move(reply);
        task->done = true;
        g_tasks_cv.notify_all();
    }
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    g_in_present.fetch_add(1);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    DrainTasks();
    const HRESULT hr = g_orig_present(swap_chain, sync_interval, flags);
    g_in_present.fetch_sub(1);
    return hr;
}

bool ExeMatchesSdk(uintptr_t base) {
    auto dos = (PIMAGE_DOS_HEADER)base;
    auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    if (stamp != sdk::kExeTimestamp) {
        Log("exe TimeDateStamp 0x%08X does not match the SDK (0x%08X); not installing", stamp, sdk::kExeTimestamp);
        return false;
    }
    return true;
}

// The swap chain vtable of a throwaway device: DXGI shares it with the game's swap chain.
void** FindPresentSlot() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"stellaris_bench_dummy";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return nullptr;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &swap_chain, &device, nullptr, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &swap_chain, &device, nullptr, &context);
    }
    void** slot = nullptr;
    if (SUCCEEDED(hr) && swap_chain) {
        slot = &(*(void***)swap_chain)[8];  // IDXGISwapChain::Present
    } else {
        Log("D3D11CreateDeviceAndSwapChain failed: 0x%08X", (unsigned)hr);
    }
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return slot;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

bool Install(uintptr_t base) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_period = 1.0 / (double)f.QuadPart;
    if (!ExeMatchesSdk(base)) return false;
    g_base = base;
    void** slot = FindPresentSlot();
    if (!slot) return false;
    g_orig_present = (FnPresent)*slot;
    if (!WriteSlot(slot, (void*)&PresentHook)) {
        Log("could not patch the Present slot at %p", (void*)slot);
        return false;
    }
    g_present_slot = slot;
    Log("patched IDXGISwapChain::Present slot %p (was %p)", (void*)slot, (void*)g_orig_present);
    return true;
}

void Uninstall() {
    if (!g_present_slot) return;
    WriteSlot(g_present_slot, (void*)g_orig_present);
    g_present_slot = nullptr;
    // a frame already inside the hook finishes its original Present call; the last instructions
    // after it are microseconds, the sleep covers them
    for (int i = 0; i < 500 && g_in_present.load() != 0; ++i) Sleep(10);
    Sleep(200);
    // wake anyone still waiting for a command; it will time out
    std::lock_guard<std::mutex> lock(g_tasks_mutex);
    g_tasks.clear();
    g_tasks_cv.notify_all();
}

std::string Execute(const std::string& line, unsigned timeout_ms) {
    if (!g_present_slot) return Error("not installed");
    auto task = std::make_shared<Task>();
    task->line = line;
    std::unique_lock<std::mutex> lock(g_tasks_mutex);
    g_tasks.push_back(task);
    if (!g_tasks_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return task->done; })) {
        // no frame rendered in time (minimized window, loading screen)
        for (auto it = g_tasks.begin(); it != g_tasks.end(); ++it) {
            if (*it == task) {
                g_tasks.erase(it);
                break;
            }
        }
        return Error("timeout: no frame rendered");
    }
    return task->reply;
}

}  // namespace bench
