#pragma once
// Engine value types whose layout is the compiler's plus one SDK offset.
#include "stellaris_sdk.hpp"

#include <string>

namespace perf {

// A CString: its own header, then the text as a std::string. The engine and the plugin are both built with MSVC's
// release runtime, so the engine's std::string is read as one. The token table's entries are CStrings, which fixes
// the size.
struct EngineString {
    char head[sdk::rt::CString_text];
    std::string text;
};
static_assert(sizeof(EngineString) == sdk::rt::CStaticLexer_entry_size, "CString layout: not the token table's entry size");

}  // namespace perf
