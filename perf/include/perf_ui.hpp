#pragma once

#include <cstdint>

namespace perf::ui {

// The engine's per-iteration frame function (CGameIdler::Idle, from the SDK): input, GUI update, the simulation tick, the GUI draw.
// RenderFrame (perf_render.hpp) calls it again from inside a tick; the main loop calls it once per iteration.
// Hooked only to measure how often it runs against the ticks.
bool Install(uintptr_t base);
// Logs the frame function's and the ticks' rates every 10 s while on.
void SetProbe(bool on);

// With this on, the RenderFrame cap (perf_render.hpp) also applies to the RenderFrame calls made between ticks, as long
// as the simulation is running (a tick ended in the last 250 ms). Those calls are about half of all RenderFrame time.
void SetCapAll(bool on);
bool CapAll();

void TickBegin();
void TickEnd();
bool Installed();

}  // namespace perf::ui
