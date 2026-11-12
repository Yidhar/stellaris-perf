#pragma once

#include <cstdint>
#include <string>

namespace perf::tooltip {

// A tooltip's content is kept instead of being built again every frame.
//
// CTooltipManager::Update runs once a frame. It resets its static CToolTip and asks for the content again: first the
// hovered GUI object's tooltip handler (GetToolTip, the handler's slot 0), then, when the object has no handler or the
// handler no tooltip, the idler's BuildToolTip (the map and what lies on it). The answer is built from scratch every
// frame, shown or not, only to be compared with the text already on screen. A ship's tooltip writes out every
// component for it (each weapon's damage lines, the cost and upkeep resource tables); a fleet's walks its ships.
//
// Both calls are virtual and made at one place each in Update. The call instructions themselves are hooked (they are
// six bytes, sdk::site): the handler getter's hands out a stand-in handler whose slot 0 is a function here, the idler's
// goes to a function here directly. Each keeps the CToolTip its provider last filled, with what the provider returned,
// and gives that back for the same provider asked about the same object as long as nothing that could change the
// content has happened:
//   * no key or mouse button pressed or released since it was built, and none for the last 100 ms (the time a
//     press's effect takes to reach the game state);
//   * the mouse where it was, as the engine reads it;
//   * the same game day and the same plugin settings;
//   * at most max_age_ms of real time.
// Everything else in Update (the hover timers, the comparison with the shown text, placing the window) runs as before,
// on the same content.
//
// Display only: while the game runs, or after a change that needs no input, a tooltip can be up to max_age_ms behind.

// Hooks the two call instructions. False, and nothing changed, when the code there is not what the SDK describes.
bool Install(uintptr_t base);
// 0 = the providers are asked every frame (the engine's behaviour), 1 = the cache, 2 = verify: the providers are asked
// every frame and what the cache would have answered is compared with it (mismatches counted, the first ones logged)
void SetMode(int mode);
void SetMaxAgeMs(int ms);
bool Installed();

struct Counters {
    uint64_t kept;          // answered from the cache (verify: the provider's answer was the cached one)
    uint64_t built;         // the provider was asked
    uint64_t bypassed;      // no input state or another thread: the provider was asked, nothing kept
    uint64_t mismatches;    // verify: the provider's answer differed from the cached one
    uint64_t build_cycles;  // TSC cycles inside the providers (every time one was asked while the cache or verify is on)
};
Counters Read();
// ms_per_cycle converts the time inside the providers (0: left out).
std::string Stats(double ms_per_cycle = 0);

// For the offline test (tests/tooltip_test.cpp), which has no engine: the hooks on two given call instructions with a
// given CString assign, and the input state and clock replaced by a source the test steps.
namespace detail {
using Sampler = bool (*)(unsigned long* last_input, long* cursor_x, long* cursor_y, uint64_t* now_ms);
bool InstallAt(uint8_t* handler_site, uint8_t* idler_site, void* assign);
void SetSampler(Sampler sample);
}  // namespace detail

}  // namespace perf::tooltip
