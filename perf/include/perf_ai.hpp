#pragma once

#include <cstdint>
#include <string>

namespace perf::ai {

// ai_parallel_lpt: the daily AI update's countries processed heaviest first.
//
// CGameState::ProcessAI runs UpdateAIParallel(CCountryAI* const* countries, int n): a parallel-for whose tasks take
// the countries one by one from a shared counter, in array order, and call each one's update (CCountryAI vtable slot
// 7, which skips countries without an AI); the main thread takes its share and then waits for the last task. Only the order the countries are taken in
// is scheduling, so with this on the hook of the per-country update processes, when the parallel-for reaches array
// slot i, the country that ranks i-th by the cost measured the day before (unknown cost ranks first): the classic
// longest-processing-time order, which keeps a big empire from being picked last and making everybody wait for it.
// Every country is still updated exactly once per phase, on whichever thread claimed its slot, and the update
// neither draws random numbers nor touches shared game state (the engine forbids both during the phase), so the
// result does not depend on the order and two clients that both run it agree. The per-country cost is measured
// whether the option is on or off; the stats line shows the phase against its heaviest country.
bool Install(uintptr_t base);
void SetEnabled(bool on);
// ai_nested_inline: a parallel-for started from inside a country's update runs on that thread (the engine's serial
// path for one task) instead of forking tasks that the other workers, busy with their own countries, do not take
// while the forking thread spin-waits. Same results: a parallel-for's chunks are order-independent.
void SetNestedInline(bool on);
bool Installed();
std::string Stats();
// For caches that live inside one AI phase: whether the calling thread is inside a country's update of the current
// phase, and the number of the current phase (bumped when one starts).
bool InCountryUpdate();
uint32_t PhaseGeneration();

}  // namespace perf::ai
