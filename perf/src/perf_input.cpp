#include "perf_input.hpp"

#include <windows.h>

namespace perf::input {
namespace {

struct State {
    uint64_t epoch = 1;
    uint64_t changed = 0;
    uint64_t sampled = 0;
    uint64_t keys = 0;  // a hash of which keys and buttons are down
};
thread_local State t_state;

}  // namespace

Actions Sample(uint64_t now_ms) {
    State& s = t_state;
    if (now_ms != s.sampled) {
        s.sampled = now_ms;
        BYTE keys[256];
        if (GetKeyboardState(keys)) {
            uint64_t down = 1469598103934665603ull;
            for (const BYTE k : keys) down = (down ^ (uint64_t)(k & 0x80)) * 1099511628211ull;
            static const int kButtons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
            bool pressed = false;
            for (const int button : kButtons) pressed |= (GetAsyncKeyState(button) & 1) != 0;
            if (s.keys == 0) {
                s.keys = down;  // the first sample is the starting point, not a change
            } else if (down != s.keys || pressed) {
                s.keys = down;
                ++s.epoch;
                s.changed = now_ms;
            }
        }
    }
    return { s.epoch, s.changed };
}

}  // namespace perf::input
