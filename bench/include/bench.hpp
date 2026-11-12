#pragma once

#include <cstdint>
#include <string>

// stellaris_bench.dll: benchmark control for Stellaris, a standalone helper for the stellaris_perf benchmark scripts.
//  * counts frames by patching IDXGISwapChain::Present in DXGI's swap chain vtable (no inline hook,
//    so it coexists with the bridge's MinHook detour of the same function in any unload order);
//  * runs one-line commands on the game's main thread at the next frame (pause, speed, status);
//  * sets the engine's UseSavedRandom console variable when it loads, so a save loaded afterwards keeps its random
//    seed and two runs of it simulate the same game (the engine otherwise reseeds a loaded game from the clock).
// Commands arrive over the pipe \\.\pipe\stellaris_bench (bench_main.cpp).
namespace bench {

void Log(const char* fmt, ...);

// Patches the Present slot; false if the exe does not match the SDK or DXGI could not be set up.
bool Install(uintptr_t base);
// Restores the slot and waits until no thread is inside the hook any more.
void Uninstall();

// Runs one command on the main thread and returns its JSON reply (one line, no newline):
//   status            {"ok":true,"in_game":..,"hours":..,"day":..,"paused":..,"speed":..,"frames":..,"t":..,
//                      "saved_random":..,"hovered":..,"mouse":..}
//   pause <0|1>       CInGameIdler::SetPaused
//   speed <0..5>      CInGameIdler::SetGameSpeed
//   saved_random <0|1>  the UseSavedRandom console variable (1 while the DLL is loaded unless changed here)
//   save <name>       CInGameIdler::SaveGame: a local save <name>.sav (1 to 15 characters) into the campaign's
//                     folder, overwriting; written over the next frames
//   select <fleet id> CInGameIdler::Select on that fleet, as a click on it: the fleet window opens on it, without the
//                     mouse; {"ok":true,"fleet":..,"ships":..}
//   mouse <x> <y>     the engine's mouse position getter answers with this client-area point from now on (hover tests,
//                     tooltips, the cursor and the map's picking follow it; the real cursor is not moved); "mouse off"
//                     gives the real cursor back. status reports "hovered" (a GUI object is under the mouse) and "mouse"
// "frames" is the Present count since the DLL loaded, "t" the QueryPerformanceCounter time in
// seconds when the command ran.
std::string Execute(const std::string& line, unsigned timeout_ms);

}  // namespace bench
