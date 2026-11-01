#pragma once

#include <cstdint>
#include <string>

namespace perf::phase {

// Phase timing per game day, bucketed by the kind of day (phase_profile=1).
//
// The main thread's time is split into the engine's phases with rdtsc at their entry points: the tick and its parts
// (micro update, daily, monthly and yearly update, the AI update including the wait for the worker threads, the
// autosave), the frames drawn inside a tick and between ticks, and the per-iteration GUI update and draw of the engine's
// frame function. Every phase gets its exclusive time (a nested phase's time is taken out of its parent's); everything
// inside RenderFrame counts as rendering. A record per game day (date, ticks, wall time, the phases) is kept and written
// to logs\stellaris_perf_phases.json, so days can be grouped: a normal day, the first of a month, the 1st of January or
// July (the half-yearly autosave). bench/scripts/phase_report.py prints the table. Measurement only; nothing changes.
enum Phase : int {
    kTick,         // CGameState::HandleTurnTick, exclusive of the phases below
    kMicro,        // CGameState::MicroUpdate
    kDaily,        // CGameState::DailyUpdate
    kMonthly,      // CGameState::MonthlyUpdate
    kYearly,       // CGameState::YearlyUpdate
    kAi,           // CGameState::ProcessAI: creates AIs, runs them on the workers and waits
    kSave,         // CSaveGameTaskManager::Update: the game-state dump of a queued (auto)save, run between ticks
    kRenderTick,   // RenderFrame called inside a tick (frame smoothing)
    kRenderFrame,  // RenderFrame called between ticks
    kFrame,        // the engine's frame function, exclusive: input, the main loop's own work
    kGui,          // in-game idler update (per frame)
    kGui2,         // GUI framework update (per frame)
    kApp,          // application update (per frame)
    kDraw,         // the draw step (per frame)
    kCount
};
extern const char* const kNames[kCount];
// Extra entry points from the ini (phase_extra = "0xRVA[=name], ..."): functions of the exe hooked while the game runs
// and timed like the fixed phases (exclusive time and call count, nested wherever they are called). Only the start of a
// function of the exe (its exception-directory entry) is accepted; a hook stays until unload and is just ignored when
// its RVA leaves the list. Main-thread calls only; the parts that run on the worker threads are not timed.
constexpr int kExtra = 128;

// Hooks the entry points that no other module hooks (each checked against its first bytes; one that does not match is
// skipped). The tick, RenderFrame and the frame function report through Enter/Exit from their own detours.
bool Install(uintptr_t base);
void SetEnabled(bool on);
bool Enabled();
void SetExtra(const char* spec);
// phase_callers = "0xRVA": for that timed function (a fixed phase's or an extra one), count its calls and cycles per
// call site (return address), separately for normal days and first-of-month days; written to the JSON as "callers".
void SetCallers(const char* spec);
// Main thread only (calls from other threads are ignored). Enter returns whether a phase was opened; Exit must get that
// value back. Inside RenderFrame every phase is swallowed, so all of it counts as rendering. p is a Phase or
// kCount + an extra slot.
bool Enter(int p);
void Exit(bool opened);
// After HandleTurnTick returned: attributes the tick to its game day and closes the day when the date moved on.
void OnTickEnd();
// Written by the plugin's worker thread every few seconds while enabled: logs\stellaris_perf_phases.json.
void WriteJson();
// One line for the statistics log: the mean of the last days, normal days against first-of-month days.
std::string Summary();

}  // namespace perf::phase
