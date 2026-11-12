#pragma once

#include <cstdint>

namespace perf::input {

// Presses and releases of keys and mouse buttons, as the calling thread's message queue last saw them. Those are what
// can change the game state from the GUI; moving the mouse cannot, and neither can input that goes to another window.
// The GUI caches (perf_tooltip.hpp, perf_gui.hpp) drop what they keep when one happens and keep nothing for a moment
// after it.
struct Actions {
    uint64_t epoch;       // moves with every press or release seen
    uint64_t changed_ms;  // GetTickCount64 of the last one, 0 before the first
};
// Samples the keyboard state (at most once a millisecond per thread) and returns the calling thread's counters. A
// mouse click shorter than the time between two samples is caught by GetAsyncKeyState's "pressed since" bit.
Actions Sample(uint64_t now_ms);

}  // namespace perf::input
