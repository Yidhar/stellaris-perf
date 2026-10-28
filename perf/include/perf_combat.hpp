#pragma once

#include <cstdint>

namespace perf::combat {

// Combat participants are shared instead of copied.
//
// Every micro tick the engine's parallel ship update (the chunk function of CShipUpdateParallelFunc) looks up the ship's
// fleet in the CombatParticipantsPerFleet table and, for a ship whose fleet has an entry, builds a deep copy of the entry's
// SCombatParticipants (seven arrays: five int arrays and two arrays of 64-byte records, each as long as the number of ships
// in the system, i.e. hundreds of KB) only to pass it as a const reference to CShip::MicroUpdateParallel, then frees it.
// This patches the copy away: the callee gets a pointer to the table's own object, and the code that would destroy the copy
// is skipped. The table is not modified while the ships are updated, and the callee only reads the argument, so every
// ship sees exactly the data it saw before.
//
// The code patch is 11 bytes, so it is only written or removed while the worker threads are idle: at the start of a tick
// (see Sync), never while a tick is running.

// Finds the code to patch by its byte pattern (unique in the executable). Returns false and changes nothing when the
// code is not what it is expected to be.
bool Install(uintptr_t base);
// Wanted state; takes effect at the start of the next tick (Sync) or in ForceRestore.
void Want(bool on);
// Main thread, start of a tick, before the engine runs it: applies a pending change.
void Sync();
// Removes the patch right now. Only call it when no tick is running.
void ForceRestore();
bool Installed();
bool Patched();

}  // namespace perf::combat
