#pragma once

#include <cstdint>

namespace perf::alloc {

// Large-block recycler for the engine's operator new / free.
//
// The engine allocates and frees its big temporary arrays (hundreds of KB: the per-fleet targeting
// tables, built and freed on every combat tick) with a plain new/free pair, so every tick takes a
// cold block from the heap and gives it back. This keeps the last few big blocks of each thread and
// hands the same block out again: a block that was just used is still in that core's cache.
//
// Only blocks of kMinBytes..the per-thread limit that go through operator new are managed; they are
// ordinary heap blocks (no header), so a block can be freed by the engine's own free at any time,
// also after the plugin is gone. A block's contents are not cleared (operator new never promised that).

constexpr uint64_t kMinBytes = 128 * 1024;

// Finds operator new, free and the CRT heap from the SDK anchors and creates the (disabled) hooks.
// Returns false, hooking nothing, when the code is not what it is expected to be.
bool Install(uintptr_t base);
// Switches the recycler on or off (off: the hooks are removed, the cached blocks go back to the heap);
// `mb_per_thread` is the most each thread keeps cached.
void Configure(bool on, int mb_per_thread);
// Before the DLL goes away: gives every cached block back to the heap. The hooks must be disabled.
void Shutdown();

struct Counters {
    uint64_t hits;       // big allocations answered from a cache
    uint64_t misses;     // big allocations that went to the heap
    uint64_t evictions;  // cached blocks given back because the cache was full
    uint64_t rejects;    // cached blocks dropped because the heap says they are too small now
    uint64_t untracked;  // blocks that could not be registered (table full); they just are not recycled
    uint64_t cached_bytes;
    uint64_t tracked;    // blocks currently registered
    bool installed;
    bool on;
};
Counters Read();

}  // namespace perf::alloc
