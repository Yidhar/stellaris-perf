#include "perf_writer.hpp"
#include "perf_tweaks.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace perf::writer {
namespace {

// From the SDK (tools/sdk_dumper/functions.py in the Stellaris MCP repository). The names are the Linux decompile's;
// the Windows build outlines the token-from-table writes into helpers that the decompile shows inlined.
constexpr uintptr_t kRvaWriteData = sdk::fn::CMemoryFile_WriteData;         // CMemoryFile::WriteData(const void*, int): vtable slot 9
constexpr uintptr_t kRvaWriteCStr = sdk::fn::CFile_WriteCStr;               // CFile::Write(const char*): slot 6, strlen then slot 9
constexpr uintptr_t kRvaGetSize = sdk::fn::CMemoryFile_GetSize;             // CMemoryFile::GetSize(): slot 16; the blob's size when there is a blob
constexpr uintptr_t kRvaWriteToken = sdk::fn::CWriter_WriteTokenText;       // CWriter::Write(CToken(id, CStaticLexer::GetString(id))): (CWriter*, int id)
constexpr uintptr_t kRvaWriteIndent = sdk::fn::CWriter_WriteIndent;         // CWriter::WriteIndent(): WriteMany(0x11, depth) in text mode
constexpr uintptr_t kRvaWriteCString = sdk::fn::CWriter_WriteCString;       // CWriter::Write(CToken(const CString&)): (CWriter*, const CString*)
constexpr uintptr_t kRvaWriteString = sdk::fn::CWriter_WriteString;         // CWriter::WriteString(const char*, bool no_eol, bool unquoted)
constexpr uintptr_t kRvaLexer = sdk::fn::CStaticLexer_Table;                // CStaticLexer's token table (a thread-safe static)
constexpr uintptr_t kRvaLexerRebuild = sdk::fn::CStaticLexer_RebuildTable;  // rebuilds the table's strings when its counts differ

// Engine layouts (Windows): a CBlob, and the fields of the memory file, the writer, the token table and a CString
// that the fast paths read.
struct Blob {
    void* vt;
    char* data;
    int32_t capacity;
    int32_t size;
};
constexpr size_t kFileBlob = 0x80;      // CBlob* (null for a file over a fixed buffer)
constexpr size_t kFilePos = 0x94;       // int32 write position
constexpr size_t kWriterDepth = 0x08;   // int32 list depth (the indentation)
constexpr size_t kWriterFile = 0x10;    // CFile*
constexpr size_t kWriterBinary = 0x18;  // bool: a binary writer
constexpr size_t kLexerEntries = 0x70;  // CString[]: the token strings by id
constexpr size_t kLexerBuilt = 0x7C;    // int32: the count the strings were built for
constexpr size_t kLexerWanted = 0x80;   // int32: the current count
constexpr int kSlotWriteCStr = 6, kSlotWriteData = 9, kSlotGetSize = 16;
constexpr int kTokenEol = 0x10, kTokenIndent = 0x11, kTokenFixedPoint = 0x167;

// The layout constants as the functions' own code uses them: each byte string (an instruction with the constant in it)
// must occur in the first `within` bytes of the function.
struct LayoutCheck {
    uintptr_t rva;
    int within;
    uint8_t bytes[8];
    int len;
    const char* name;
};
const LayoutCheck kLayoutChecks[] = {
    { kRvaWriteData, 32, { 0x48, 0x83, 0xB9, (uint8_t)kFileBlob, 0, 0, 0, 0 }, 8, "CMemoryFile::WriteData: cmp qword ptr [rcx + blob], 0" },
    { kRvaGetSize, 16, { 0x48, 0x8B, 0x81, (uint8_t)kFileBlob, 0, 0, 0 }, 7, "CMemoryFile::GetSize: mov rax, [rcx + blob]" },
    { kRvaWriteIndent, 48, { 0x80, 0x79, (uint8_t)kWriterBinary, 0 }, 4, "CWriter::WriteIndent: cmp byte ptr [rcx + binary], 0" },
    { kRvaWriteCStr, 32, { 0xFF, 0x60, (uint8_t)(8 * kSlotWriteData) }, 3, "CFile::Write(const char*): jmp [rax + WriteData slot]" },
};

// A CString: something at +0, the std::string at +0x10 (the token table's entries are CStrings too).
struct CString {
    char head[0x10];
    union {
        char* ptr;
        char sso[16];
    };
    size_t size;
    size_t capacity;
    const char* Text() const { return capacity >= 16 ? ptr : sso; }
};
static_assert(sizeof(CString) == 48, "CString layout");

using WriteDataFn = void (*)(void*, const void*, int);
using WriteTokenFn = void (*)(void*, int);
using WriteIndentFn = void (*)(void*);
using WriteCStringFn = void (*)(void*, const CString*);
using WriteStringFn = void (*)(void*, const char*, bool, bool);
using LexerFn = char* (*)();
using RebuildFn = void (*)();

WriteDataFn g_orig_write_data = nullptr;
WriteTokenFn g_orig_write_token = nullptr;
WriteIndentFn g_orig_write_indent = nullptr;
WriteCStringFn g_orig_write_cstring = nullptr;
WriteStringFn g_orig_write_string = nullptr;
LexerFn g_lexer = nullptr;
RebuildFn g_rebuild = nullptr;
void* g_write_cstr = nullptr;  // CFile::Write(const char*) as the slot-6 value of a plain file
void* g_get_size = nullptr;    // CMemoryFile::GetSize as the slot-16 value of a memory file over a blob
bool g_installed = false;
std::atomic<bool> g_on{ false };

// Counts per thread, without atomics: the serialization makes tens of millions of these calls per save.
struct Counters {
    uint64_t appends, tokens, indents, labels, strings;
};
constexpr int kThreads = 256;
Counters g_pool[kThreads];
Counters g_overflow;  // the threads beyond the pool share this one (approximate)
std::atomic<int> g_nthreads{ 0 };
thread_local Counters* t_counters = nullptr;

Counters& C() {
    Counters* c = t_counters;
    if (!c) {
        const int i = g_nthreads.fetch_add(1);
        c = i < kThreads ? &g_pool[i] : &g_overflow;
        t_counters = c;
    }
    return *c;
}

inline void** VTable(const void* obj) { return *(void***)obj; }
inline bool Binary(const void* w) { return *((const uint8_t*)w + kWriterBinary) != 0; }
inline void* File(const void* w) { return *(void**)((const char*)w + kWriterFile); }
inline int Depth(const void* w) { return *(const int32_t*)((const char*)w + kWriterDepth); }
// A file whose Write(const char*) is the plain one (strlen, then WriteData): then WriteData with the known length is
// the same call. Anything else keeps the engine's path.
inline bool PlainFile(const void* file) { return VTable(file)[kSlotWriteCStr] == g_write_cstr; }
inline void FileWrite(void* file, const void* data, int n) { ((WriteDataFn)VTable(file)[kSlotWriteData])(file, data, n); }

// The token table's entry for an id, the table rebuilt first when the engine would (its counts differ).
inline const CString* Token(int id) {
    char* lexer = g_lexer();
    if (*(int32_t*)(lexer + kLexerBuilt) != *(int32_t*)(lexer + kLexerWanted)) g_rebuild();
    return *(const CString**)(lexer + kLexerEntries) + id;
}

// Most writes are one to a few bytes: no call into the runtime for those.
inline void Copy(char* dst, const char* src, int n) {
    if (n > 16) {
        memcpy(dst, src, (size_t)n);
    } else if (n >= 8) {
        uint64_t a, b;
        memcpy(&a, src, 8);
        memcpy(&b, src + n - 8, 8);
        memcpy(dst, &a, 8);
        memcpy(dst + n - 8, &b, 8);
    } else if (n >= 4) {
        uint32_t a, b;
        memcpy(&a, src, 4);
        memcpy(&b, src + n - 4, 4);
        memcpy(dst, &a, 4);
        memcpy(dst + n - 4, &b, 4);
    } else {
        dst[0] = src[0];
        dst[n - 1] = src[n - 1];
        if (n == 3) dst[1] = src[1];
    }
}

// CMemoryFile::WriteData: the engine asks the file its size (virtual), overwrites what lies between the position and
// the end (nothing when appending), and appends the rest through CBlob::Append (grow check, runtime memcpy), then
// advances the position. Appending inside the blob's capacity is the common case: done here in place.
void WriteDataDetour(void* self, const void* data, int n) {
    if (g_on.load(std::memory_order_relaxed) && VTable(self)[kSlotGetSize] == g_get_size) {
        Blob* blob = *(Blob**)((char*)self + kFileBlob);
        if (blob) {
            const int32_t pos = *(int32_t*)((char*)self + kFilePos);
            if (n > 0 && pos == blob->size && n <= blob->capacity - blob->size) {
                Copy(blob->data + pos, (const char*)data, n);
                blob->size = pos + n;
                *(int32_t*)((char*)self + kFilePos) = pos + n;
                C().appends++;
                return;
            }
        }
    }
    g_orig_write_data(self, data, n);
}

// A token by id, text mode: the engine copies the table's string into a temporary CToken (a 256-byte hybrid array
// with a grow check) and writes it through Write(CToken), which writes a C string. The table's strings come from C
// strings, so the length is the string's.
void WriteTokenDetour(void* w, int id) {
    if (g_on.load(std::memory_order_relaxed) && !Binary(w) && id != kTokenFixedPoint) {
        void* file = File(w);
        if (PlainFile(file)) {
            const CString* t = Token(id);
            FileWrite(file, t->Text(), (int)t->size);
            C().tokens++;
            return;
        }
    }
    g_orig_write_token(w, id);
}

// The indentation: the engine writes the indent token depth times, each through the path above. One write of a run
// of the token's byte when it is a single byte (a tab), the token depth times otherwise.
constexpr int kRun = 4096;
char g_run[kRun];
std::atomic<char> g_run_char{ 0 };  // the byte the run holds, once filled; 0 = not yet
std::mutex g_run_mutex;

void WriteIndentDetour(void* w) {
    if (g_on.load(std::memory_order_relaxed) && !Binary(w)) {
        void* file = File(w);
        if (PlainFile(file)) {
            const int depth = Depth(w);
            if (depth <= 0) return;
            const CString* t = Token(kTokenIndent);
            const char* s = t->Text();
            const int len = (int)t->size;
            if (len == 1 && depth <= kRun && s[0] != 0) {
                char have = g_run_char.load(std::memory_order_acquire);
                if (have == 0) {
                    std::lock_guard<std::mutex> lock(g_run_mutex);
                    if (g_run_char.load(std::memory_order_relaxed) == 0) {
                        memset(g_run, s[0], sizeof g_run);
                        g_run_char.store(s[0], std::memory_order_release);
                    }
                    have = g_run_char.load(std::memory_order_acquire);
                }
                if (have == s[0]) {
                    FileWrite(file, g_run, depth);
                    C().indents++;
                    return;
                }
            }
            for (int i = 0; i < depth; ++i) FileWrite(file, s, len);
            C().indents++;
            return;
        }
    }
    g_orig_write_indent(w);
}

// A string token from a CString (keys): the engine copies size bytes and a terminator into a temporary CToken and
// writes it as a C string, so the bytes up to the first NUL within the size.
void WriteCStringDetour(void* w, const CString* s) {
    if (g_on.load(std::memory_order_relaxed) && !Binary(w)) {
        void* file = File(w);
        if (PlainFile(file)) {
            const char* p = s->Text();
            FileWrite(file, p, (int)strnlen(p, s->size));
            C().labels++;
            return;
        }
    }
    g_orig_write_cstring(w, s);
}

// CWriter::WriteString(const char*, no_eol, unquoted), text mode. Unquoted: the string as it is. Quoted: the engine
// writes a quote, copies the string into a CString, replaces every backslash and quote by its escape (two scans),
// writes it and the closing quote; without those characters the three writes are one. Then the end-of-line token
// unless no_eol. A string with an escape takes the engine's path.
constexpr size_t kQuoted = 1024;

void WriteStringDetour(void* w, const char* s, bool no_eol, bool unquoted) {
    if (g_on.load(std::memory_order_relaxed) && !Binary(w) && s) {
        void* file = File(w);
        if (PlainFile(file)) {
            if (unquoted) {
                FileWrite(file, s, (int)strlen(s));
            } else {
                const char* p = s;
                while (*p && *p != '"' && *p != '\\') ++p;
                if (*p) {
                    g_orig_write_string(w, s, no_eol, unquoted);
                    return;
                }
                const size_t n = (size_t)(p - s);
                if (n + 2 <= kQuoted) {
                    char buf[kQuoted];
                    buf[0] = '"';
                    memcpy(buf + 1, s, n);
                    buf[n + 1] = '"';
                    FileWrite(file, buf, (int)(n + 2));
                } else {
                    FileWrite(file, "\"", 1);
                    FileWrite(file, s, (int)n);
                    FileWrite(file, "\"", 1);
                }
            }
            if (!no_eol) WriteTokenDetour(w, kTokenEol);
            C().strings++;
            return;
        }
    }
    g_orig_write_string(w, s, no_eol, unquoted);
}

bool CheckLayout(uintptr_t base) {
    for (const LayoutCheck& c : kLayoutChecks) {
        const uint8_t* p = (const uint8_t*)(base + c.rva);
        bool found = false;
        for (int i = 0; i + c.len <= c.within && !found; ++i) found = memcmp(p + i, c.bytes, (size_t)c.len) == 0;
        if (!found) {
            Log("writer: %s not found at RVA 0x%llX; save_fast_writer unavailable", c.name, (unsigned long long)c.rva);
            return false;
        }
    }
    return true;
}

}  // namespace

bool Install(uintptr_t base) {
    if (!CheckLayout(base)) return false;
    g_lexer = (LexerFn)(base + kRvaLexer);
    g_rebuild = (RebuildFn)(base + kRvaLexerRebuild);
    g_write_cstr = (void*)(base + kRvaWriteCStr);
    g_get_size = (void*)(base + kRvaGetSize);
    struct HookSpec {
        uintptr_t rva;
        void* detour;
        void** orig;
        const char* name;
    } hooks[] = {
        { kRvaWriteData, (void*)&WriteDataDetour, (void**)&g_orig_write_data, "CMemoryFile::WriteData" },
        { kRvaWriteToken, (void*)&WriteTokenDetour, (void**)&g_orig_write_token, "CWriter::WriteToken" },
        { kRvaWriteIndent, (void*)&WriteIndentDetour, (void**)&g_orig_write_indent, "CWriter::WriteIndent" },
        { kRvaWriteCString, (void*)&WriteCStringDetour, (void**)&g_orig_write_cstring, "CWriter::Write(CString)" },
        { kRvaWriteString, (void*)&WriteStringDetour, (void**)&g_orig_write_string, "CWriter::WriteString" },
    };
    for (const HookSpec& h : hooks) {
        if (MH_CreateHook((LPVOID)(base + h.rva), h.detour, h.orig) != MH_OK) {
            Log("writer: could not hook %s; save_fast_writer unavailable", h.name);
            return false;
        }
    }
    for (const HookSpec& h : hooks) {
        if (MH_EnableHook((LPVOID)(base + h.rva)) != MH_OK) {
            Log("writer: could not enable the hook on %s; save_fast_writer unavailable", h.name);
            return false;
        }
    }
    g_installed = true;
    Log("writer: hooked CMemoryFile::WriteData and the writer's token, indent, key and string writes (save_fast_writer)");
    return true;
}

void SetEnabled(bool on) {
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("save_fast_writer=%d", on ? 1 : 0);
}

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "save_fast_writer=unavailable";
    Counters sum = g_overflow;
    const int n = g_nthreads.load();
    for (int i = 0; i < n && i < kThreads; ++i) {
        sum.appends += g_pool[i].appends;
        sum.tokens += g_pool[i].tokens;
        sum.indents += g_pool[i].indents;
        sum.labels += g_pool[i].labels;
        sum.strings += g_pool[i].strings;
    }
    char buf[200];
    snprintf(buf, sizeof buf, "save_fast_writer=%s(appends=%llu tokens=%llu indents=%llu labels=%llu strings=%llu)",
             g_on.load() ? "on" : "off", (unsigned long long)sum.appends, (unsigned long long)sum.tokens,
             (unsigned long long)sum.indents, (unsigned long long)sum.labels, (unsigned long long)sum.strings);
    return std::string(buf);
}

}  // namespace perf::writer
