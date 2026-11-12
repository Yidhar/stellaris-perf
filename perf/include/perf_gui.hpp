#pragma once

#include <cstdint>
#include <string>

namespace perf::gui {

// Two answers the in-game GUI asks the simulation for again every frame, kept for a short while. Both are hooks on
// the engine function that computes the answer, and only the calls made from the GUI's own call sites (told apart by
// the return address, sdk::site) are answered from the cache; every other caller gets the engine's computation.
// Display only: what the simulation does never depends on them.
//
// The fleet window's upgrade button (fleet_upgrade_cache). CFleetActionUpgradeButton::IsValid runs every frame for
// every selected fleet and calls NFleetUtil::CanAffordUpgrade: the upgrade cost of every ship of the fleet, component
// by component (resource tables), against the owner's treasury and income, with the localized cost text as the
// reason. The button's call gets the last answer (result and reason text) back for the same fleet with the same ship
// array and ship count, on the same game day, as long as no key or mouse button was pressed or released since it was
// computed (and none in the 100 ms before; moving the mouse changes nothing), for at most one second. The upgrade
// command's own check is not touched.
//
// The cursor and the hover indicator in the galaxy view (cursor_path_cache). With fleets of the player's selected and
// the mouse over a system, CInGameIdler::UpdateCursor (for the first selected fleet) and the hovered system icon's
// indicator (for every selected fleet) call CFleetMovementManager::CalcCanMoveTo every frame: a FindPath across the
// galaxy unless the fleet's current path already leads there. Those two calls get the last answer for the same fleet
// and system back on the same game day, for at most half a second.
//
// Modes: 0 = off, 1 = on, 2 = verify (the engine answers every time and the kept answer is compared with it).

// Hooks both functions (they pass through while their mode is 0). False when a hook could not be placed.
bool Install(uintptr_t base);
void SetUpgradeMode(int mode);
void SetPathMode(int mode);
bool Installed();

struct Counters {
    uint64_t upgrade_kept, upgrade_asked, upgrade_mismatches, upgrade_cycles;  // cycles: TSC inside the engine's check
    uint64_t path_kept, path_asked, path_mismatches, path_cycles;
};
Counters Read();
std::string Stats(double ms_per_cycle);

}  // namespace perf::gui
