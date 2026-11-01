#pragma once

#include <cstdint>
#include <string>

namespace perf::writer {

// save_fast_writer: the text of a save file put into the memory file without the engine's per-token detours.
//
// A save is serialized as text by one thread while the game waits. Each piece of it (indentation, key, "=", value,
// newline) becomes a temporary CToken (copy into a 256-byte hybrid array, grow check), is written through
// CWriter::Write(CToken) as a C string (strlen), and lands in CMemoryFile::WriteData, which asks the file its size
// through a virtual call, calls CBlob::Append, which calls the grow check and the C runtime's memcpy, for one to a
// few bytes each time; a quoted string is first copied into a CString and scanned twice for the characters to
// escape; indentation is one such write per tab. In the sampled profile that machinery, not the formatting, is
// the larger half of the serialization. With this on:
// - CMemoryFile::WriteData appends in place when the blob has room (the engine's path otherwise);
// - a token, a key and the indentation are written straight from the engine's own token table, the indentation
//   as one run of tabs;
// - a quoted string without a quote or backslash in it is written with its quotes in one piece, the engine's
//   escaping path otherwise.
// The bytes written are the same; the fast paths only replace copies and calls. Each hooked function keeps the
// engine's own behaviour for binary writers and for files that are not plain memory files.
bool Install(uintptr_t base);
void SetEnabled(bool on);
bool Installed();
std::string Stats();

}  // namespace perf::writer
