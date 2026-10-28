#pragma once

#include <cstdint>

namespace perf::render {

// Frame rate cap for the frames the engine draws in the middle of a tick (frame smoothing).
//
// With frame smoothing on the engine renders a frame from inside the simulation (the checks sit in the micro-tick loops
// and between the daily/monthly phases) whenever its refresh-rate based timer says one is due; on a high-refresh monitor
// that is up to the monitor's rate, and every such frame walks and draws the whole GUI window tree on the simulation
// thread. This hooks the engine's RenderFrame (the one function all those sites call) and drops the calls made inside a
// tick that come closer together than 1 / fps seconds. The frames between ticks are not touched. Display only.

// Finds RenderFrame from the call sites' byte pattern (every site must call the same function) and hooks it.
bool Install(uintptr_t base);
// 0 = the game's own rate.
void SetFps(int fps);
// The tick detour reports where a tick starts and ends (main thread).
void TickBegin();
void TickEnd();
bool Installed();
uint64_t Dropped();
uint64_t Passed();

}  // namespace perf::render
