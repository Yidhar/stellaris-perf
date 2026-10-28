#include "perf_combat.hpp"
#include "perf_tweaks.hpp"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace perf::combat {
namespace {

uint8_t* g_site = nullptr;          // start of the copy sequence (the `lea rax, [vtable]` after the empty-entry test)
uint8_t g_orig[11];
uint8_t g_patch[11];
std::atomic<bool> g_want{ false };
std::atomic<bool> g_patched{ false };

struct Pat {
    const uint8_t* bytes;
    const uint8_t* mask;  // 1 = must match
    size_t n;
};

bool Matches(const uint8_t* p, const Pat& pat) {
    for (size_t i = 0; i < pat.n; ++i) {
        if (pat.mask[i] && p[i] != pat.bytes[i]) return false;
    }
    return true;
}

// (anonymous namespace)::CShipUpdateParallelFunc::operator()(range): after the fleet's entry was found in the table,
//   cmp byte ptr [rbx+4], 0FFh ; je empty        -- no entry: an empty SCombatParticipants is built on the stack
//   lea rax, [rip+vtable] ...                    -- copy: seven CPdxArray objects on the stack, filled by InsertAt
//   ... lea r8, [rbp-30h] ; or r14d, 1 ; xor ebx, ebx ; jmp common   -- r8 = the argument, bit 0 of r14d = "destroy the copy"
//   common: mov [rbp+0B8h], r14d ; ... call CShip::MicroUpdateParallel(ship, seed, r8)
// The patch replaces the start of the copy by   lea r8, [rbx+10h] ; xor ebx, ebx ; jmp common   (rbx = the table entry,
// its SCombatParticipants starts at +10h), and leaves bit 0 of r14d clear, so the destructors of the copy are skipped.
const uint8_t kSiteBytes[] = {
    0x80, 0x7B, 0x04, 0xFF,                    // cmp byte ptr [rbx+4], 0FFh
    0x0F, 0x84, 0, 0, 0, 0,                    // je empty
    0x48, 0x8D, 0x05, 0, 0, 0, 0,              // lea rax, [rip+vtable]
    0x48, 0x89, 0x45, 0xD0,                    // mov [rbp-30h], rax
    0x33, 0xC0,                                // xor eax, eax
    0x48, 0x89, 0x45, 0xD8,                    // mov [rbp-28h], rax
    0x48, 0x89, 0x45, 0xE0,                    // mov [rbp-20h], rax
    0x44, 0x8B, 0x4B, 0x24,                    // mov r9d, [rbx+24h]
    0x4C, 0x8B, 0x43, 0x18,                    // mov r8, [rbx+18h]
    0x33, 0xD2,                                // xor edx, edx
    0x48, 0x8D, 0x4D, 0xD0,                    // lea rcx, [rbp-30h]
    0xE8,                                      // call InsertAt
};
const uint8_t kSiteMask[] = {
    1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
static_assert(sizeof(kSiteBytes) == sizeof(kSiteMask), "pattern and mask differ in length");
constexpr size_t kSiteSkip = 10;  // the copy starts after `cmp byte [rbx+4],0FFh ; je`

const uint8_t kTail[] = { 0x4C, 0x8D, 0x45, 0xD0, 0x41, 0x83, 0xCE, 0x01, 0x33, 0xDB, 0xE9 };  // lea r8,[rbp-30h]; or r14d,1; xor ebx,ebx; jmp
const uint8_t kCommon[] = { 0x44, 0x89, 0xB5, 0xB8, 0x00, 0x00, 0x00 };                         // mov [rbp+0B8h], r14d

bool TextRange(uintptr_t base, uint8_t** begin, uint8_t** end) {
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            *begin = (uint8_t*)(base + sec[i].VirtualAddress);
            *end = *begin + sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    VirtualProtect(at, n, old, &old);
    return true;
}

}  // namespace

bool Install(uintptr_t base) {
    uint8_t *b, *e;
    if (!TextRange(base, &b, &e)) {
        Log("combat: no .text section");
        return false;
    }
    const Pat pat{ kSiteBytes, kSiteMask, sizeof kSiteBytes };
    uint8_t* found = nullptr;
    int count = 0;
    for (uint8_t* p = b; p + pat.n < e; ++p) {
        if (p[0] == kSiteBytes[0] && p[1] == kSiteBytes[1] && p[2] == kSiteBytes[2] && Matches(p, pat)) {
            found = p;
            ++count;
        }
    }
    if (count != 1) {
        Log("combat: the copy of SCombatParticipants was found %d times (expected once); sharing stays off", count);
        return false;
    }
    uint8_t* site = found + kSiteSkip;
    // the tail of the copy sequence: lea r8,[rbp-30h]; or r14d,1; xor ebx,ebx; jmp common
    uint8_t* tail = nullptr;
    int tails = 0;
    for (uint8_t* p = site; p < site + 0x300; ++p) {
        if (memcmp(p, kTail, sizeof kTail) == 0) {
            tail = p;
            ++tails;
        }
    }
    if (tails != 1) {
        Log("combat: the end of the copy sequence was found %d times (expected once); sharing stays off", tails);
        return false;
    }
    const int32_t rel = *(const int32_t*)(tail + sizeof kTail);
    uint8_t* common = tail + sizeof kTail + 4 + rel;
    if (common < b || common + sizeof kCommon > e || memcmp(common, kCommon, sizeof kCommon) != 0) {
        Log("combat: the join point after the copy is not what it should be; sharing stays off");
        return false;
    }
    const ptrdiff_t jrel = common - (site + 11);
    if (jrel < INT32_MIN || jrel > INT32_MAX) return false;
    memcpy(g_orig, site, sizeof g_orig);
    static const uint8_t head[6] = { 0x4C, 0x8D, 0x43, 0x10, 0x33, 0xDB };  // lea r8,[rbx+10h]; xor ebx,ebx
    memcpy(g_patch, head, 6);
    g_patch[6] = 0xE9;
    const int32_t j = (int32_t)jrel;
    memcpy(g_patch + 7, &j, 4);
    g_site = site;
    Log("combat: copy of SCombatParticipants at RVA 0x%llX (%u bytes), joins at 0x%llX", (unsigned long long)((uintptr_t)site - base),
        (unsigned)sizeof g_patch, (unsigned long long)((uintptr_t)common - base));
    return true;
}

void Want(bool on) { g_want.store(on); }

void Sync() {
    if (!g_site) return;
    const bool want = g_want.load();
    if (want == g_patched.load()) return;
    if (WriteCode(g_site, want ? g_patch : g_orig, sizeof g_orig)) {
        g_patched.store(want);
        Log("combat: SCombatParticipants copy %s", want ? "removed (the argument is the table's own object)" : "restored");
    }
}

void ForceRestore() {
    if (!g_site || !g_patched.load()) return;
    if (WriteCode(g_site, g_orig, sizeof g_orig)) g_patched.store(false);
}

bool Installed() { return g_site != nullptr; }
bool Patched() { return g_patched.load(); }

}  // namespace perf::combat
