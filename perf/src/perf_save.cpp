#include "perf_save.hpp"
#include "perf_tweaks.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace perf::save {
namespace {

// From the SDK (tools/sdk_dumper/functions.py in the Stellaris MCP repository): the engine's variadic thunks and the
// runtime they call
constexpr uintptr_t kRvaSprintf = sdk::fn::CRT_sprintf;               // int sprintf(char* buf, const char* fmt, ...)
constexpr uintptr_t kRvaSnprintf = sdk::fn::CRT_snprintf;             // int snprintf(char* buf, size_t n, const char* fmt, ...)
constexpr uintptr_t kRvaCore = sdk::fn::CRT_stdio_common_vsprintf;    // __stdio_common_vsprintf(options, buf, count, fmt, locale, va_list)
constexpr uintptr_t kRvaOptions = sdk::fn::CRT_stdio_printf_options;  // returns &__local_stdio_printf_options
constexpr uint64_t kSprintfOption = 1;   // the thunks OR these into the runtime's options
constexpr uint64_t kSnprintfOption = 2;

using CoreFn = int (*)(uint64_t, char*, size_t, const char*, void*, va_list);
using OptionsFn = uint64_t* (*)();
using SprintfFn = int (*)(char*, const char*, ...);
using SnprintfFn = int (*)(char*, size_t, const char*, ...);
CoreFn g_core = nullptr;
OptionsFn g_options = nullptr;
SprintfFn g_orig_sprintf = nullptr;
SnprintfFn g_orig_snprintf = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{ false };
std::atomic<uint64_t> g_fast{ 0 }, g_slow{ 0 };

enum Format { kD, kU, kLld, kS, kFixed, kPadLld, kLldDotS, kNegLldDotS, kCount };
const char* const kFormats[kCount] = { "%d", "%u", "%lld", "%s", "%s%lld.%0.5d", "%0*lld", "%lld.%s", "-%lld.%s" };
bool g_ok[kCount];

int Which(const char* fmt) {
    if (fmt[0] != '%' && fmt[0] != '-') return -1;
    for (int i = 0; i < kCount; ++i) {
        if (strcmp(fmt, kFormats[i]) == 0) return i;
    }
    return -1;
}

// Decimal digits of v into the end of a buffer; returns the first digit.
char* Digits(uint64_t v, char* end) {
    char* p = end;
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return p;
}

// "%lld" with an optional minimum number of digits (the printf precision: zeros before the digits, after the sign)
// and an optional minimum width filled with zeros (the printf 0 flag: zeros after the sign up to the width).
int PutInt(char* out, int64_t v, int precision, int width) {
    char tmp[32];
    const uint64_t mag = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    char* d = Digits(mag, tmp + sizeof tmp);
    int ndigits = (int)(tmp + sizeof tmp - d);
    int zeros = precision > ndigits ? precision - ndigits : 0;
    const int sign = v < 0 ? 1 : 0;
    if (width > sign + zeros + ndigits) zeros = width - sign - ndigits;
    char* p = out;
    if (sign) *p++ = '-';
    for (int i = 0; i < zeros; ++i) *p++ = '0';
    memcpy(p, d, (size_t)ndigits);
    return (int)(p + ndigits - out);
}

int PutStr(char* out, const char* s) {
    const size_t n = strlen(s);
    memcpy(out, s, n);
    return (int)n;
}

// Formats into out (room for any of these: a string is copied whole) and returns the length, or -1 for a format
// that is not one of ours.
int Fast(int which, char* out, va_list va) {
    switch (which) {
    case kD: return PutInt(out, va_arg(va, int), 0, 0);
    case kU: {
        char tmp[32];
        char* d = Digits(va_arg(va, unsigned), tmp + sizeof tmp);
        const int n = (int)(tmp + sizeof tmp - d);
        memcpy(out, d, (size_t)n);
        return n;
    }
    case kLld: return PutInt(out, va_arg(va, long long), 0, 0);
    case kS: return PutStr(out, va_arg(va, const char*));
    case kFixed: {
        const char* s = va_arg(va, const char*);
        const long long whole = va_arg(va, long long);
        const int frac = va_arg(va, int);
        int n = PutStr(out, s);
        n += PutInt(out + n, whole, 0, 0);
        out[n++] = '.';
        n += PutInt(out + n, frac, 5, 0);
        return n;
    }
    case kPadLld: {
        const int width = va_arg(va, int);
        const long long v = va_arg(va, long long);
        return PutInt(out, v, 0, width);
    }
    case kLldDotS:
    case kNegLldDotS: {
        const long long v = va_arg(va, long long);
        const char* s = va_arg(va, const char*);
        int n = 0;
        if (which == kNegLldDotS) out[n++] = '-';
        n += PutInt(out + n, v, 0, 0);
        out[n++] = '.';
        n += PutStr(out + n, s);
        return n;
    }
    }
    return -1;
}

// A string argument can be long: the fast path formats into a local buffer only when it fits, which is always for
// the numeric formats and for the short strings the writer passes (yes / no, the sign of a fixed point).
constexpr int kLocal = 128;

bool StringFits(int which, va_list va) {
    va_list copy;
    va_copy(copy, va);
    bool fits = true;
    if (which == kS || which == kFixed) fits = strlen(va_arg(copy, const char*)) < 64;
    else if (which == kLldDotS || which == kNegLldDotS) {
        (void)va_arg(copy, long long);
        fits = strlen(va_arg(copy, const char*)) < 64;
    }
    va_end(copy);
    return fits;
}

int SprintfDetour(char* buf, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    int r;
    const int which = g_on.load(std::memory_order_relaxed) ? Which(fmt) : -1;
    if (which >= 0 && g_ok[which] && StringFits(which, va)) {
        char tmp[kLocal];
        r = Fast(which, tmp, va);
        memcpy(buf, tmp, (size_t)r);
        buf[r] = 0;
        g_fast.fetch_add(1, std::memory_order_relaxed);
    } else {
        r = g_core(*g_options() | kSprintfOption, buf, (size_t)-1, fmt, nullptr, va);
        if (r < 0) r = -1;
        g_slow.fetch_add(1, std::memory_order_relaxed);
    }
    va_end(va);
    return r;
}

int SnprintfDetour(char* buf, size_t n, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    int r;
    const int which = g_on.load(std::memory_order_relaxed) ? Which(fmt) : -1;
    if (which >= 0 && g_ok[which] && StringFits(which, va)) {
        char tmp[kLocal];
        r = Fast(which, tmp, va);
        // C99 snprintf: at most n - 1 characters and a terminator, the full length returned
        if (n) {
            const size_t keep = (size_t)r < n ? (size_t)r : n - 1;
            memcpy(buf, tmp, keep);
            buf[keep] = 0;
        }
        g_fast.fetch_add(1, std::memory_order_relaxed);
    } else {
        r = g_core(*g_options() | kSnprintfOption, buf, n, fmt, nullptr, va);
        if (r < 0) r = -1;
        g_slow.fetch_add(1, std::memory_order_relaxed);
    }
    va_end(va);
    return r;
}

// The check at install: every fast format against the runtime, byte for byte, on a battery of values.
int Runtime(uint64_t option, char* buf, size_t n, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    const int r = g_core(*g_options() | option, buf, n, fmt, nullptr, va);
    va_end(va);
    return r;
}

int FastVar(int which, char* out, ...) {
    va_list va;
    va_start(va, out);
    const int r = Fast(which, out, va);
    va_end(va);
    out[r] = 0;
    return r;
}

bool Same(const char* a, int ra, const char* b, int rb) { return ra == rb && strcmp(a, b) == 0; }

void SelfTest() {
    const long long ll[] = { 0, 1, -1, 7, 42, -42, 99999, 100000, -100000, 123456789, -987654321, 2147483647LL, -2147483648LL,
                             4294967295LL, 9007199254740993LL, 9223372036854775807LL, (-9223372036854775807LL - 1) };
    const int ints[] = { 0, 1, -1, 9, 10, 99, 100, 12345, -12345, 2147483647, (-2147483647 - 1) };
    const char* strs[] = { "", "yes", "no", "-", "a b", "x\"y" };
    char a[kLocal], b[kLocal];
    bool ok[kCount];
    for (int i = 0; i < kCount; ++i) ok[i] = true;
    for (int v : ints) {
        if (!Same(a, FastVar(kD, a, v), b, Runtime(kSprintfOption, b, (size_t)-1, "%d", v))) ok[kD] = false;
        if (!Same(a, FastVar(kU, a, (unsigned)v), b, Runtime(kSprintfOption, b, (size_t)-1, "%u", (unsigned)v))) ok[kU] = false;
        for (int w = 0; w <= 7; ++w) {
            if (!Same(a, FastVar(kPadLld, a, w, (long long)v), b, Runtime(kSnprintfOption, b, 64, "%0*lld", w, (long long)v))) ok[kPadLld] = false;
        }
    }
    for (long long v : ll) {
        if (!Same(a, FastVar(kLld, a, v), b, Runtime(kSprintfOption, b, (size_t)-1, "%lld", v))) ok[kLld] = false;
        for (int w = 0; w <= 7; ++w) {
            if (!Same(a, FastVar(kPadLld, a, w, v), b, Runtime(kSnprintfOption, b, 64, "%0*lld", w, v))) ok[kPadLld] = false;
        }
        for (const char* s : strs) {
            for (int f : ints) {
                if (!Same(a, FastVar(kFixed, a, s, v, f), b, Runtime(kSnprintfOption, b, 64, "%s%lld.%0.5d", s, v, f))) ok[kFixed] = false;
            }
            if (!Same(a, FastVar(kLldDotS, a, v, s), b, Runtime(kSnprintfOption, b, 64, "%lld.%s", v, s))) ok[kLldDotS] = false;
            if (!Same(a, FastVar(kNegLldDotS, a, v, s), b, Runtime(kSnprintfOption, b, 64, "-%lld.%s", v, s))) ok[kNegLldDotS] = false;
        }
    }
    for (const char* s : strs) {
        if (!Same(a, FastVar(kS, a, s), b, Runtime(kSprintfOption, b, (size_t)-1, "%s", s))) ok[kS] = false;
    }
    int passed = 0;
    for (int i = 0; i < kCount; ++i) {
        g_ok[i] = ok[i];
        if (ok[i]) ++passed;
        else Log("save: fast format \"%s\" differs from the runtime; left to the runtime", kFormats[i]);
    }
    Log("save: %d of %d fast formats match the runtime", passed, kCount);
}

}  // namespace

bool Install(uintptr_t base) {
    g_core = (CoreFn)(base + kRvaCore);
    g_options = (OptionsFn)(base + kRvaOptions);
    SelfTest();
    const LPVOID sp = (LPVOID)(base + kRvaSprintf), snp = (LPVOID)(base + kRvaSnprintf);
    if (MH_CreateHook(sp, (LPVOID)&SprintfDetour, (LPVOID*)&g_orig_sprintf) != MH_OK ||
        MH_CreateHook(snp, (LPVOID)&SnprintfDetour, (LPVOID*)&g_orig_snprintf) != MH_OK || MH_EnableHook(sp) != MH_OK ||
        MH_EnableHook(snp) != MH_OK) {
        Log("save: could not hook the sprintf / snprintf thunks; save_fast_format unavailable");
        return false;
    }
    g_installed = true;
    Log("save: hooked the sprintf and snprintf thunks (save_fast_format)");
    return true;
}

void SetEnabled(bool on) {
    if (!g_installed) return;
    if (g_on.exchange(on) != on) Log("save_fast_format=%d", on ? 1 : 0);
}

bool Installed() { return g_installed; }

std::string Stats() {
    if (!g_installed) return "save_fast_format=unavailable";
    char buf[120];
    snprintf(buf, sizeof buf, "save_fast_format=%s(fast=%llu runtime=%llu)", g_on.load() ? "on" : "off", (unsigned long long)g_fast.load(),
             (unsigned long long)g_slow.load());
    return std::string(buf);
}

}  // namespace perf::save
