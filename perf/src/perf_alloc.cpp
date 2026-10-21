#include "perf_alloc.hpp"
#include "perf_tweaks.hpp"
#include "stellaris_sdk.hpp"
#include "MinHook.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace perf::alloc {
namespace {

constexpr uint32_t kStep = 64 * 1024;     // capacities are multiples of this
constexpr int kEntries = 8;               // cached blocks per thread
constexpr int kMaxCached = 1024;          // thread caches ever created (they are reused)
constexpr int kMaxPerThreadMb = 64;

// ---- the set of blocks this recycler handed out --------------------------------------------------------
// word = (address >> 4) << 12 | capacity / kStep; 0 = empty, 1 = removed. Every free() of the process asks
// this table whether the block is one of ours, so it is small (64 KB) and a lookup is one or two loads.
// A block missing from the table is only a block that is not recycled: it is freed like any other.
constexpr uint32_t kSetBits = 13, kSetSize = 1u << kSetBits, kProbe = 64;
std::atomic<uint64_t> g_set[kSetSize];
std::atomic<int64_t> g_tracked{ 0 };

inline uint32_t SetHash(uintptr_t p) {
    return (uint32_t)(((uint64_t)(p >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kSetBits));
}
inline uint64_t SetKey(uintptr_t p) { return (uint64_t)(p >> 4) << 12; }

inline uint32_t SetFind(uintptr_t p) {
    const uint64_t key = SetKey(p);
    const uint32_t h = SetHash(p);
    for (uint32_t k = 0; k < kProbe; ++k) {
        const uint64_t v = g_set[(h + k) & (kSetSize - 1)].load(std::memory_order_acquire);
        if (v > 1 && (v & ~0xFFFull) == key) return (uint32_t)(v & 0xFFF) * kStep;
        if (v == 0) return 0;
    }
    return 0;
}

bool SetInsert(uintptr_t p, uint32_t cap) {
    const uint64_t key = SetKey(p), val = key | (cap / kStep);
    const uint32_t h = SetHash(p);
    int free_at = -1;
    for (uint32_t k = 0; k < kProbe; ++k) {
        std::atomic<uint64_t>& s = g_set[(h + k) & (kSetSize - 1)];
        const uint64_t v = s.load(std::memory_order_acquire);
        if (v > 1 && (v & ~0xFFFull) == key) {  // an old entry for this address (the block came back some other way)
            s.store(val, std::memory_order_release);
            return true;
        }
        if (v <= 1 && free_at < 0) free_at = (int)k;
        if (v == 0) break;
    }
    if (free_at < 0) return false;
    for (uint32_t k = (uint32_t)free_at; k < kProbe; ++k) {
        std::atomic<uint64_t>& s = g_set[(h + k) & (kSetSize - 1)];
        uint64_t v = s.load(std::memory_order_acquire);
        if (v <= 1 && s.compare_exchange_strong(v, val, std::memory_order_acq_rel)) {
            g_tracked.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

// Clears the entry. A slot that ends a probe chain becomes empty again (with the removed slots before it),
// so removals do not slowly fill the table; a racing insert can at worst make a block unrecycled.
void SetRemove(uintptr_t p) {
    const uint64_t key = SetKey(p);
    const uint32_t h = SetHash(p);
    for (uint32_t k = 0; k < kProbe; ++k) {
        const uint32_t i = (h + k) & (kSetSize - 1);
        uint64_t v = g_set[i].load(std::memory_order_acquire);
        if (v > 1 && (v & ~0xFFFull) == key) {
            if (!g_set[i].compare_exchange_strong(v, 1, std::memory_order_acq_rel)) return;
            g_tracked.fetch_sub(1, std::memory_order_relaxed);
            if (g_set[(i + 1) & (kSetSize - 1)].load(std::memory_order_acquire) == 0) {
                for (uint32_t j = i, n = 0; n < kProbe; ++n, j = (j - 1) & (kSetSize - 1)) {
                    uint64_t t = 1;
                    if (!g_set[j].compare_exchange_strong(t, 0, std::memory_order_acq_rel)) break;
                }
            }
            return;
        }
        if (v == 0) return;
    }
}

void SetClear() {
    for (auto& s : g_set) s.store(0, std::memory_order_release);
    g_tracked.store(0, std::memory_order_relaxed);
}

// ---- per-thread caches ---------------------------------------------------------------------------------
struct Ent {
    uintptr_t ptr;
    uint32_t cap;
};

struct TL {
    std::atomic<uint32_t> lock;  // taken on every operation; flushing from another thread takes it too
    int n;
    uint64_t bytes;
    Ent ent[kEntries];           // oldest first
    uint64_t hits, misses, evictions, rejects, untracked;
};

using NewFn = void* (*)(size_t);
using FreeFn = void (*)(void*);
NewFn g_orig_new = nullptr;
FreeFn g_orig_free = nullptr;
uintptr_t g_new_addr = 0, g_free_addr = 0;
HANDLE g_heap = nullptr;
bool g_installed = false;
std::atomic<bool> g_on{ false };
std::atomic<uint64_t> g_limit{ 8ull << 20 };  // bytes cached per thread

// Caches are found by thread id (one load of the TEB; Win32 TLS/FLS getters would reset the thread's last error,
// and `thread_local` is not safe to touch while a thread is being torn down, which is when the CRT frees memory).
// A cache stays with its thread id for good: a new thread that gets the same id simply shares it (every
// operation takes the cache's lock).
constexpr uint32_t kTidSlots = 1024;
struct TidSlot {
    std::atomic<uint32_t> tid;  // 0 = empty
    TL* tl;
};
TidSlot g_tids[kTidSlots];
SRWLOCK g_reg_lock = SRWLOCK_INIT;
TL* g_tl[kMaxCached];
std::atomic<int> g_ntl{ 0 };
DWORD g_fls = FLS_OUT_OF_INDEXES;  // only for its thread-exit callback

inline void Lock(TL* t) {
    while (t->lock.exchange(1, std::memory_order_acquire)) _mm_pause();
}
inline void Unlock(TL* t) { t->lock.store(0, std::memory_order_release); }

// Gives the cached blocks back to the heap.
void Flush(TL* t) {
    Ent out[kEntries];
    int n;
    Lock(t);
    n = t->n;
    memcpy(out, t->ent, sizeof(Ent) * (size_t)n);
    t->n = 0;
    t->bytes = 0;
    Unlock(t);
    for (int i = 0; i < n; ++i) {
        SetRemove(out[i].ptr);
        g_orig_free((void*)out[i].ptr);
    }
}

void WINAPI OnThreadExit(PVOID data) {
    TL* t = (TL*)data;
    if (t && g_installed) Flush(t);  // nothing stays cached for a thread that is gone
}

inline uint32_t TidHash(uint32_t tid) { return (tid * 2654435761u) >> 22; }  // 10 bits

TL* Lookup(uint32_t tid) {
    for (uint32_t k = 0, h = TidHash(tid); k < kTidSlots; ++k) {
        const uint32_t cur = g_tids[(h + k) & (kTidSlots - 1)].tid.load(std::memory_order_acquire);
        if (cur == tid) return g_tids[(h + k) & (kTidSlots - 1)].tl;
        if (cur == 0) return nullptr;
    }
    return nullptr;
}

TL* Acquire(uint32_t tid) {
    const DWORD last_error = GetLastError();
    TL* t = nullptr;
    AcquireSRWLockExclusive(&g_reg_lock);
    if ((t = Lookup(tid)) == nullptr) {
        const int n = g_ntl.load();
        if (n < kMaxCached) {
            t = (TL*)VirtualAlloc(nullptr, sizeof(TL), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (t) {
                g_tl[n] = t;
                g_ntl.store(n + 1);
                for (uint32_t k = 0, h = TidHash(tid); k < kTidSlots; ++k) {
                    TidSlot& s = g_tids[(h + k) & (kTidSlots - 1)];
                    if (s.tid.load() == 0) {
                        s.tl = t;
                        s.tid.store(tid, std::memory_order_release);
                        break;
                    }
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_reg_lock);
    if (t && g_fls != FLS_OUT_OF_INDEXES) FlsSetValue(g_fls, t);  // so OnThreadExit gets the cache
    SetLastError(last_error);
    return t;
}

inline TL* Tls() {
    const uint32_t tid = GetCurrentThreadId();
    TL* t = Lookup(tid);
    return t ? t : Acquire(tid);
}

void FlushAll() {
    const int n = g_ntl.load();
    for (int i = 0; i < n; ++i) Flush(g_tl[i]);
}

// ---- the detours ---------------------------------------------------------------------------------------
void* BigAlloc(size_t size) {
    TL* t = Tls();
    const uint32_t cap = (uint32_t)((size + kStep - 1) & ~(size_t)(kStep - 1));
    if (!t || cap > g_limit.load(std::memory_order_relaxed)) return g_orig_new(size);  // too big to ever be cached
    void* hit = nullptr;
    Lock(t);
    for (int i = 0; i < t->n; ++i) {
        const uint32_t c = t->ent[i].cap;
        if (c == cap || c == cap + kStep) {
            hit = (void*)t->ent[i].ptr;
            t->bytes -= c;
            for (int j = i + 1; j < t->n; ++j) t->ent[j - 1] = t->ent[j];
            --t->n;
            break;
        }
    }
    Unlock(t);
    if (hit) {
        const SIZE_T have = HeapSize(g_heap, 0, hit);
        if (have != (SIZE_T)-1 && have >= size) {
            ++t->hits;
            return hit;
        }
        // the block is not what it was when it was cached (resized by someone else): never reuse it
        ++t->rejects;
        SetRemove((uintptr_t)hit);
        g_orig_free(hit);
    }
    void* p = g_orig_new(cap);
    if (p && !SetInsert((uintptr_t)p, cap)) ++t->untracked;
    ++t->misses;
    return p;
}

// true: the block was one of ours and has been cached or given back
bool BigFree(void* p) {
    const uint32_t cap = SetFind((uintptr_t)p);
    if (!cap) return false;
    TL* t = g_on.load(std::memory_order_relaxed) ? Tls() : nullptr;
    const uint64_t limit = g_limit.load(std::memory_order_relaxed);
    if (!t || cap > limit) {
        SetRemove((uintptr_t)p);
        g_orig_free(p);
        return true;
    }
    Ent evict[kEntries];
    int ne = 0;
    Lock(t);
    while (t->n > 0 && (t->n == kEntries || t->bytes + cap > limit)) {  // the oldest go first
        evict[ne++] = t->ent[0];
        t->bytes -= t->ent[0].cap;
        for (int j = 1; j < t->n; ++j) t->ent[j - 1] = t->ent[j];
        --t->n;
    }
    t->ent[t->n++] = { (uintptr_t)p, cap };
    t->bytes += cap;
    t->evictions += (uint64_t)ne;
    Unlock(t);
    for (int i = 0; i < ne; ++i) {
        SetRemove(evict[i].ptr);
        g_orig_free((void*)evict[i].ptr);
    }
    return true;
}

void* NewDetour(size_t size) {
    // kMinBytes <= size < 256 MB: one compare for every other allocation in the process
    if (size - kMinBytes < (256ull << 20) - kMinBytes && g_on.load(std::memory_order_relaxed)) return BigAlloc(size);
    return g_orig_new(size);
}

void FreeDetour(void* p) {
    if (p && BigFree(p)) return;
    g_orig_free(p);
}

// ---- finding the functions -----------------------------------------------------------------------------
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    return mbi.State == MEM_COMMIT && (const uint8_t*)p + n <= (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}

// Follows `jmp rel32` thunks and the stub `mov [rsp+10h],0; mov eax,[rsp+10h]; jmp rel32` the linker puts in
// front of free.
const uint8_t* Follow(const uint8_t* p) {
    static const uint8_t stub[12] = { 0xC7, 0x44, 0x24, 0x10, 0, 0, 0, 0, 0x8B, 0x44, 0x24, 0x10 };
    for (int i = 0; i < 6; ++i) {
        if (!Readable(p, 18)) break;
        if (p[0] == 0xE9) {
            p = p + 5 + *(const int32_t*)(p + 1);
        } else if (memcmp(p, stub, sizeof stub) == 0 && p[12] == 0xE9) {
            p = p + 17 + *(const int32_t*)(p + 13);
        } else {
            break;
        }
    }
    return p;
}


// operator new: `push rbx; sub rsp,20h; mov rbx,rcx`, its allocation call leads (through thunks) to malloc,
// which loads the CRT heap handle from a global with `mov rcx,[rip+disp32]`.
HANDLE FindCrtHeap(const uint8_t* op_new) {
    const uint8_t* malloc_fn = nullptr;
    for (int i = 0; i < 0x30 && !malloc_fn; ++i) {
        if (op_new[i] != 0xE8) continue;
        const uint8_t* target = Follow(op_new + i + 5 + *(const int32_t*)(op_new + i + 1));
        // malloc: push rbx; sub rsp,20h; mov rbx,rcx; cmp rcx,-20h
        static const uint8_t head[13] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xF9, 0xE0 };
        if (Readable(target, 0x60) && memcmp(target, head, sizeof head) == 0) malloc_fn = target;
    }
    if (!malloc_fn) return nullptr;
    for (int i = 0; i < 0x50; ++i) {
        if (malloc_fn[i] == 0x48 && malloc_fn[i + 1] == 0x8B && malloc_fn[i + 2] == 0x0D) {
            const uint8_t* slot = malloc_fn + i + 7 + *(const int32_t*)(malloc_fn + i + 3);
            if (!Readable(slot, sizeof(HANDLE))) return nullptr;
            return *(HANDLE*)slot;
        }
    }
    return nullptr;
}

}  // namespace

bool Install(uintptr_t base) {
    const uint8_t* op_new = (const uint8_t*)(base + sdk::kRvaEngineAlloc);
    const uint8_t* op_del = (const uint8_t*)(base + sdk::fn::CRT_operator_delete);
    // push rbx; sub rsp,20h; mov rbx,rcx
    static const uint8_t new_head[9] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    if (memcmp(op_new, new_head, sizeof new_head) != 0) {
        Log("allocator: operator new does not look as expected; the recycler stays off");
        return false;
    }
    const uint8_t* free_fn = Follow(op_del);
    // free: test rcx,rcx; je ...
    if (!(free_fn[0] == 0x48 && free_fn[1] == 0x85 && free_fn[2] == 0xC9 && free_fn[3] == 0x74)) {
        Log("allocator: operator delete does not lead to free as expected; the recycler stays off");
        return false;
    }
    g_heap = FindCrtHeap(op_new);
    if (!g_heap) {
        Log("allocator: the CRT heap handle was not found; the recycler stays off");
        return false;
    }
    {   // the handle must be a heap that knows its blocks' sizes
        void* probe = HeapAlloc(g_heap, 0, 24);
        const SIZE_T size = probe ? HeapSize(g_heap, 0, probe) : (SIZE_T)-1;
        if (probe) HeapFree(g_heap, 0, probe);
        if (size == (SIZE_T)-1 || size < 24) {
            Log("allocator: the CRT heap handle does not answer HeapSize; the recycler stays off");
            return false;
        }
    }
    g_new_addr = (uintptr_t)op_new;
    g_free_addr = (uintptr_t)free_fn;
    g_fls = FlsAlloc(&OnThreadExit);
    if (g_fls == FLS_OUT_OF_INDEXES) {
        Log("allocator: no FLS slot; the recycler stays off");
        return false;
    }
    if (MH_CreateHook((LPVOID)g_new_addr, (LPVOID)&NewDetour, (LPVOID*)&g_orig_new) != MH_OK ||
        MH_CreateHook((LPVOID)g_free_addr, (LPVOID)&FreeDetour, (LPVOID*)&g_orig_free) != MH_OK) {
        Log("allocator: could not create the hooks; the recycler stays off");
        FlsFree(g_fls);
        g_fls = FLS_OUT_OF_INDEXES;
        return false;
    }
    g_installed = true;
    Log("allocator: operator new 0x%llX, free 0x%llX, CRT heap %p", (unsigned long long)(g_new_addr - base),
        (unsigned long long)(g_free_addr - base), g_heap);
    return true;
}

void Configure(bool on, int mb_per_thread) {
    if (!g_installed) return;
    if (mb_per_thread < 0) mb_per_thread = 0;
    if (mb_per_thread > kMaxPerThreadMb) mb_per_thread = kMaxPerThreadMb;
    g_limit.store((uint64_t)mb_per_thread << 20);
    on = on && mb_per_thread > 0;
    if (on == g_on.load()) return;
    if (on) {
        g_on.store(true);
        // free first, so a block handed out by new can always come back through the hook
        MH_EnableHook((LPVOID)g_free_addr);
        MH_EnableHook((LPVOID)g_new_addr);
        Log("allocator: recycler on, %d MB per thread", mb_per_thread);
    } else {
        g_on.store(false);
        MH_DisableHook((LPVOID)g_new_addr);
        MH_DisableHook((LPVOID)g_free_addr);
        FlushAll();
        SetClear();  // the blocks still out are plain heap blocks: they are freed the ordinary way
        Log("allocator: recycler off");
    }
}

void Shutdown() {
    if (!g_installed) return;
    g_on.store(false);
    FlushAll();
    SetClear();
    g_installed = false;
    if (g_fls != FLS_OUT_OF_INDEXES) {
        FlsFree(g_fls);
        g_fls = FLS_OUT_OF_INDEXES;
    }
    const int n = g_ntl.load();
    for (int i = 0; i < n; ++i) {
        VirtualFree(g_tl[i], 0, MEM_RELEASE);
        g_tl[i] = nullptr;
    }
    g_ntl.store(0);
}

Counters Read() {
    Counters c{};
    c.installed = g_installed;
    c.on = g_on.load();
    const int n = g_ntl.load();
    for (int i = 0; i < n; ++i) {
        const TL* t = g_tl[i];
        if (!t) continue;
        c.hits += t->hits;
        c.misses += t->misses;
        c.evictions += t->evictions;
        c.rejects += t->rejects;
        c.untracked += t->untracked;
        c.cached_bytes += t->bytes;
    }
    const int64_t tr = g_tracked.load();
    c.tracked = tr > 0 ? (uint64_t)tr : 0;
    return c;
}

}  // namespace perf::alloc
