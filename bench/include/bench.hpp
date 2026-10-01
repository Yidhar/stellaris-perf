#pragma once

#include <cstdint>
#include <string>

// stellaris_bench.dll: benchmark control for Stellaris, a standalone helper for the stellaris_perf benchmark scripts.
//  * counts frames by patching IDXGISwapChain::Present in DXGI's swap chain vtable (no inline hook,
//    so it coexists with the bridge's MinHook detour of the same function in any unload order);
//  * runs one-line commands on the game's main thread at the next frame (pause, speed, status).
// Commands arrive over the pipe \\.\pipe\stellaris_bench (bench_main.cpp).
namespace bench {

void Log(const char* fmt, ...);

// Patches the Present slot; false if the exe does not match the SDK or DXGI could not be set up.
bool Install(uintptr_t base);
// Restores the slot and waits until no thread is inside the hook any more.
void Uninstall();

// Runs one command on the main thread and returns its JSON reply (one line, no newline):
//   status            {"ok":true,"in_game":..,"hours":..,"day":..,"paused":..,"speed":..,"frames":..,"t":..}
//   pause <0|1>       CInGameIdler::SetPaused
//   speed <0..5>      CInGameIdler::SetGameSpeed
// "frames" is the Present count since the DLL loaded, "t" the QueryPerformanceCounter time in
// seconds when the command ran.
std::string Execute(const std::string& line, unsigned timeout_ms);

}  // namespace bench
