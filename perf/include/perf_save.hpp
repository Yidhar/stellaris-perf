#pragma once

#include <cstdint>
#include <string>

namespace perf::save {

// save_fast_format: the numbers of a save file formatted without the C runtime's printf engine.
//
// A save is written as text by one thread while the game waits (0.6 s on a late save): every number goes through the
// engine's sprintf / snprintf thunks into the C runtime's printf machinery (CToken(int) "%d", (unsigned) "%u",
// CWriter::Write(CToken) "%s%lld.%0.5d" for fixed points, CToken(CFixedPoint) "%0*lld" and "%lld.%s"), which is about
// a quarter of the serialization in the sampled profile. With this on, the two thunks are hooked and those few
// formats are produced by plain integer-to-decimal code; every other format goes to the runtime as before. The text
// is identical: at install each fast format is checked against the runtime on a battery of values (zero, signs,
// extremes, paddings) and is only used if every byte and the return value match.
bool Install(uintptr_t base);
void SetEnabled(bool on);
bool Installed();
std::string Stats();

}  // namespace perf::save
