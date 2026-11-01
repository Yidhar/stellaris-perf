#pragma once

#include <cstdint>
#include <string>

namespace perf::weight {

// ai_weight_cache: script weight factors memoised within one daily AI phase.
//
// The AI's choices (ship design components, buildings and districts, techs, resolutions, operations) are scored by
// scripted weights (`ai_weight`, `weight`, `ai_will_do` blocks: a base and modifiers with triggers), all evaluated by
// CMeanTimeToHappen::GetRawFactor(weight, out, scope). One ship design alone evaluates every candidate component's
// weight for every slot it could go into, twice, so the same (weight, scope) pair is evaluated many times in one
// country's update, and the heaviest empire's update spends about a tenth of its time there. During the parallel AI
// phase the game state does not change (the engine closes global state access and forbids random numbers), so the
// same pair gives the same factor all phase long. With this on, each thread keeps a table of (weight, scope
// identity) -> factor for the phase in progress (the scope's self, from, root and prev objects; scopes with
// parameters or saved event targets are not cached) and answers repeats from it. The engine's results are unchanged,
// and the cache is used only inside a country's update of the phase (the player's designer, tooltips and events
// evaluate as before).
bool Install(uintptr_t base);
void SetEnabled(bool on);
bool Installed();
std::string Stats();

}  // namespace perf::weight
