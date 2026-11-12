#include "perf_nodes.hpp"
#include "perf_engine.hpp"
#include "perf_shared.hpp"
#include "perf_tweaks.hpp"
#include "MinHook.h"
#include "stellaris_sdk.hpp"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace perf::nodes {
namespace {

// From the SDK (tools/sdk_dumper/functions.py in the Stellaris MCP repository)
constexpr uintptr_t kRvaInvalidateChildren = sdk::fn::CModifierNodeBase_InvalidateChildren;  // CModifierNodeBase::InvalidateChildren(node, mask)
constexpr uintptr_t kRvaModifierClear = sdk::fn::CModifier_ClearEntries;  // the node modifier's virtual Clear (vtable slot 16): the first step of every rebuild
constexpr uintptr_t kCascadeSite = sdk::site::CModifierNodeBase_Invalidate_AddInvalid + 5;  // the return address of AddInvalid inside the per-child Invalidate
// Layouts, all from the SDK (tools/sdk_dumper/anchors.py reads each out of the engine code that uses it)
constexpr ptrdiff_t kNodeId = sdk::rt::CModifierNode_id;        // -1 = not registered
constexpr ptrdiff_t kNodeCalc = sdk::rt::CModifierNode_calc;    // the std::function impl whose _Do_call(impl, node) runs the owner's calc
constexpr int kCalcCallSlot = sdk::vt::CModifierNodeCalc_Call;  // _Do_call in the impl's vtable
constexpr ptrdiff_t kNodeDirty = sdk::rt::CModifierNode_dirty;
constexpr ptrdiff_t kTypeCategory = sdk::rt::CModifierNodeType_category;  // of the node type the node points at (its first member)
constexpr ptrdiff_t kPlanetColony = sdk::ent::CPlanet::colony;  // the colony ref (0xFFFFFFFF = none)
constexpr ptrdiff_t kSlotStride = sdk::rt::CModifierNodeManager_slot_size;
constexpr uint32_t kIndexMask = (uint32_t)sdk::rt::CModifierNodeManager_index_mask;  // a node id's slot index
// a node's content: the modifier's entries (value, then type) and parent records (clone, multiplier, name, category)
constexpr ptrdiff_t kEntrySize = sdk::rt::CModifier_entry_size, kEntryType = sdk::rt::CModifierEntry_type;
constexpr ptrdiff_t kParentSize = sdk::rt::CModifier_parent_size, kParentMult = sdk::rt::CModifierParent_multiplier;
constexpr ptrdiff_t kParentName = sdk::rt::CModifierParent_name, kParentCat = sdk::rt::CModifierParent_category;
constexpr int kMaxCat = 64;
constexpr int kInvSlots = 4096;
constexpr int kNodeSlots = 1 << 17;  // node states (kept across windows: the parents' last rebuilds are consulted)
constexpr int kMaxDoCall = 48;
constexpr uint32_t kWindowDays = 30;
constexpr uint32_t kMaxWalks = 50000;  // stack walks per window (root invalidations)

using InvFn = void (*)(void*, uint32_t);
using ClearFn = void (*)(void*, void*);
using DoCallFn = uint64_t (*)(void*, void*);
InvFn g_orig_inv = nullptr;
ClearFn g_orig_clear = nullptr;
uintptr_t g_base = 0;
bool g_installed = false;
std::atomic<int> g_trace{ 0 };
std::atomic<bool> g_in_flush{ false };
std::atomic<void*> g_mgr{ nullptr };
std::atomic<uint32_t> g_main_tid{ 0 };
double g_ms_per_cycle = 0;
uintptr_t g_text0 = 0, g_text1 = 0;  // the exe's .text, from its section table

// the invalidation table: one row per (site, category, parent category, root site, root category, kind)
struct InvKey {
    uint32_t site, root_site, root_site2;
    uint8_t cat, parent_cat, root_cat, kind;  // kind: bit 0 = inside the monthly update, bit 1 = a root record
};
struct InvRow {
    std::atomic<uint64_t> key_a, key_b;  // key_a = site | root_site << 32 | bit 63 claimed; key_b = cat | parent_cat << 8 | root_cat << 16 | kind << 24 | root_site2 << 32
    std::atomic<uint64_t> n;
};
InvRow g_inv[kInvSlots];
std::atomic<uint64_t> g_inv_overflow{ 0 };
std::atomic<uint32_t> g_walks{ 0 };

// per node: how it was invalidated since its last rebuild (bit 0 = directly, bit 1 = by a cascade, bit 2 = more cascade
// parents than kept), the parents that cascaded to it and when, and its last rebuild: when and whether it changed
constexpr int kMaxParents = 4;
struct NodeState {
    std::atomic<uintptr_t> node;
    std::atomic<uint32_t> flags;
    std::atomic<uint32_t> nparents;
    std::atomic<uintptr_t> parent[kMaxParents];
    std::atomic<uint32_t> parent_seq[kMaxParents];
    std::atomic<uint32_t> rebuild_seq;
    std::atomic<uint32_t> changed;  // 0 = unknown, 1 = unchanged, 2 = changed
    std::atomic<uint32_t> pre_valid;  // pre_hash = the content when the node was first invalidated after its last rebuild
    std::atomic<uint64_t> pre_hash;
    std::atomic<uint64_t> last_hash;  // the content after the last rebuild
    std::atomic<uint64_t> parent_hash[kMaxParents];  // the parent's content when it first cascaded here: what this node was built from
    std::atomic<uint32_t> pre_copy;   // 1 + the index of a copy of the entries taken at the invalidation (diagnostics), 0 = none
};

// copies of a node's entries and parent records at its invalidation, to show what a rebuild changed when no parent
// did (a pool per category, so every category gets its share)
constexpr int kPreCopies = 256, kPreEntries = 48, kPreParents = 8;
struct PreCopy {
    uint32_t n, parents;
    uint64_t val[kPreEntries];
    uint32_t type[kPreEntries];
    uint64_t pmult[kPreParents], pname[kPreParents], pclone[kPreParents];
    uint32_t pcat[kPreParents];
    char pstr[kPreParents][12];  // the first bytes behind the name pointer, when readable
};
PreCopy g_pre[kMaxCat][kPreCopies];
std::atomic<int> g_pre_n[kMaxCat];
std::atomic<uint32_t> g_diff_logged[kMaxCat];
constexpr uint32_t kDiffPerCat = 12;

bool CopyEntries(const void* node, PreCopy* out) {
    __try {
        const unsigned char* m = (const unsigned char*)node + sdk::rt::CModifierNode_modifier;
        const int n = *(const int32_t*)(m + sdk::rt::CModifier_entry_count);
        if (n < 0 || n > kPreEntries) return false;
        const unsigned char* e = *(unsigned char* const*)(m + sdk::rt::CModifier_entries);
        for (int i = 0; i < n; ++i) {
            out->val[i] = *(const uint64_t*)(e + i * kEntrySize);
            out->type[i] = *(const uint32_t*)(e + i * kEntrySize + kEntryType);
        }
        out->n = (uint32_t)n;
        const int pn = *(const int32_t*)(m + sdk::rt::CModifier_parent_count);
        out->parents = (uint32_t)pn;
        const unsigned char* pr = *(unsigned char* const*)(m + sdk::rt::CModifier_parents);
        for (int i = 0; i < pn && i < kPreParents; ++i) {
            out->pclone[i] = *(const uint64_t*)(pr + i * kParentSize);
            out->pmult[i] = *(const uint64_t*)(pr + i * kParentSize + kParentMult);
            out->pname[i] = *(const uint64_t*)(pr + i * kParentSize + kParentName);
            out->pcat[i] = *(const uint32_t*)(pr + i * kParentSize + kParentCat);
            memset(out->pstr[i], 0, sizeof out->pstr[i]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ReadNameBytes(PreCopy* c) {
    for (uint32_t i = 0; i < c->parents && i < (uint32_t)kPreParents; ++i) {
        __try {
            const char* p = (const char*)c->pname[i];
            if (p) memcpy(c->pstr[i], p, 11);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            c->pstr[i][0] = '?';
        }
    }
}

uint32_t NodeIdOf(const void* node) {
    __try {
        return *(const uint32_t*)((const unsigned char*)node + kNodeId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
}

// A modifier type's key, the way the engine prints it (CModifier::LogDefinitions): the definition's token, whose text
// is the token table's entry. The definitions array (sdk::glob::CModifier_Definitions) keeps its count next to its
// data pointer; the token table is a static object (sdk::glob::CStaticLexer_Instance). All offsets are the SDK's.
constexpr uintptr_t kRvaModifierDefinitions = sdk::glob::CModifier_Definitions;
bool g_defs_ok = false;

bool ReadTypeName(uint32_t type, char* out, size_t cap_out) {
    __try {
        const unsigned char* arr = *(unsigned char* const*)(g_base + kRvaModifierDefinitions);
        const uint32_t count = *(const uint32_t*)(g_base + kRvaModifierDefinitions + sdk::rt::CModifierDefinitions_count);
        if (!arr || type >= count) return false;
        const int32_t token = *(const int32_t*)(arr + (size_t)type * sdk::rt::CModifierDefinition_size + sdk::rt::CModifierDefinition_token);
        const unsigned char* lexer = (const unsigned char*)(g_base + sdk::glob::CStaticLexer_Instance);
        const EngineString* entries = *(const EngineString* const*)(lexer + sdk::rt::CStaticLexer_entries);
        if (!entries || token < 0 || token >= *(const int32_t*)(lexer + sdk::rt::CStaticLexer_built)) return false;
        const size_t size = entries[token].text.size();
        if (size == 0 || size >= cap_out) return false;
        memcpy(out, entries[token].text.data(), size);
        out[size] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string TypeName(uint32_t type) {
    char buf[64];
    if (!g_defs_ok || !ReadTypeName(type, buf, sizeof buf)) return "";
    return buf;
}

// the difference between the copy and the node's entries now, as a log line
void LogDiff(int cat, const void* node, const PreCopy& pre) {
    PreCopy now;
    if (!CopyEntries(node, &now)) return;
    char buf[400];
    int len = snprintf(buf, sizeof buf, "node_trace: diff cat=%d node=0x%X entries %u->%u parents %u->%u:", cat, NodeIdOf(node), pre.n, now.n, pre.parents, now.parents);
    int shown = 0;
    for (uint32_t i = 0; i < pre.n && shown < 6 && len < (int)sizeof buf - 60; ++i) {
        uint32_t j = 0;
        for (; j < now.n; ++j)
            if (now.type[j] == pre.type[i]) break;
        if (j == now.n) len += snprintf(buf + len, sizeof buf - (size_t)len, " t%u(%s) %lld->gone;", pre.type[i], TypeName(pre.type[i]).c_str(), (long long)pre.val[i]), ++shown;
        else if (now.val[j] != pre.val[i]) len += snprintf(buf + len, sizeof buf - (size_t)len, " t%u(%s) %lld->%lld;", pre.type[i], TypeName(pre.type[i]).c_str(), (long long)pre.val[i], (long long)now.val[j]), ++shown;
    }
    for (uint32_t j = 0; j < now.n && shown < 6 && len < (int)sizeof buf - 60; ++j) {
        uint32_t i = 0;
        for (; i < pre.n; ++i)
            if (pre.type[i] == now.type[j]) break;
        if (i == pre.n) len += snprintf(buf + len, sizeof buf - (size_t)len, " t%u(%s) new %lld;", now.type[j], TypeName(now.type[j]).c_str(), (long long)now.val[j]), ++shown;
    }
    if (!shown) {
        len += snprintf(buf + len, sizeof buf - (size_t)len, " entries equal;");
        ReadNameBytes(&now);
        for (uint32_t i = 0; i < now.parents && i < (uint32_t)kPreParents && len < (int)sizeof buf - 90; ++i) {
            const bool same = i < pre.parents && pre.pmult[i] == now.pmult[i] && pre.pname[i] == now.pname[i] && pre.pcat[i] == now.pcat[i];
            char s[12];
            memcpy(s, now.pstr[i], 11);
            s[11] = 0;
            for (char& ch : s)
                if (ch && (ch < 32 || ch > 126)) ch = '.';
            len += snprintf(buf + len, sizeof buf - (size_t)len, " p%u%s mult %lld->%lld name %llx->%llx(%s) cat %u->%u;", i, same ? "=" : "!",
                            i < pre.parents ? (long long)pre.pmult[i] : -1, (long long)now.pmult[i], i < pre.parents ? (unsigned long long)pre.pname[i] : 0ull,
                            (unsigned long long)now.pname[i], s, i < pre.parents ? pre.pcat[i] : 0u, now.pcat[i]);
        }
    }
    Log("%s", buf);
}
NodeState g_nf[kNodeSlots];
std::atomic<uint64_t> g_nf_overflow{ 0 };
std::atomic<uint32_t> g_seq{ 1 };  // every invalidation and rebuild moves it

// rebuilds: [where][monthly][category]; where: 0 = on demand, main thread; 1 = on demand, a worker; 2 = the flush.
// For a node invalidated only by cascades: whether every parent that cascaded to it was rebuilt afterwards without
// a change (parents_ok): ok_unchanged = the rebuild could have been skipped; ok_changed = it could not (its own state
// changed without an invalidation, or an untracked parent); notok_* = a parent changed, was not rebuilt, or is unknown
struct CatStat {
    std::atomic<uint64_t> n, cycles, changed, unchanged, unknown, direct, cascade_only, unflagged;
    std::atomic<uint64_t> ok_unchanged, ok_changed, notok_unchanged, notok_changed;
};
CatStat g_cat[3][2][kMaxCat];
// the flush: [monthly]
std::atomic<uint64_t> g_flush_n[2], g_flush_cycles[2];
// planet rebuilds [where][monthly][colonized] and invalidations [cascade][monthly][colonized]
std::atomic<uint64_t> g_planet_rb[3][2][2];
std::atomic<uint64_t> g_planet_inv[2][2][2];
std::atomic<int> g_planet_cat{ -1 };
std::atomic<ptrdiff_t> g_planet_off{ 0 };  // planet - node, learned from the first planet rebuild
std::atomic<uint64_t> g_planet_learn_fail{ 0 };
uint32_t g_day0 = 0, g_last_day = 0;
uint64_t g_days = 0, g_dumps = 0;

// the calc delegates (one std::function _Do_call per owner class), discovered from the nodes and hooked on the main
// thread between ticks
struct DoCall {
    std::atomic<uintptr_t> fn;
    std::atomic<int> cat;
    std::atomic<bool> installed;
    DoCallFn orig;
};
DoCall g_docall[kMaxDoCall];
std::atomic<int> g_docall_n{ 0 };
std::atomic<uint64_t> g_docall_overflow{ 0 };
// per category: the owner's calc the delegate jumps to, to name the category. The std::function's impl holds, after
// its vtable pointer, the bound callable: the owner, then the member function pointer (MSVC's layout of a callable,
// not an engine structure)
std::atomic<uintptr_t> g_cat_calc[kMaxCat];

uintptr_t CalcOf(const void* impl) {
    __try {
        return *(const uintptr_t*)((const unsigned char*)impl + 2 * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint64_t NodeHash(const void* node);
void LearnDoCall(const void* node);
bool DelegateHooked(const void* node);

// ---- modifier_cascade_gate ----
// The eager cascade of CCountry::InvalidateModifier (the country's main node) and of the relay network node's
// invalidation (the country's daily update, the relay network update) is held: the node is rebuilt at once, on the
// invalidating thread, right after the engine registers it, and its children are invalidated the engine's own way
// (InvalidateChildren with every category) only when the rebuilt content differs from the content they were built
// from. The masked cascades the rebuild itself makes are dropped meanwhile (they find no linked child in the engine
// either, which unlinks the children at the invalidation). The held nodes' rebuild delegates (the std::function
// _Do_call of CCountry's nodes and the relay manager's) are learned from the nodes at their first held invalidation
// and hooked at the next day change; until then the engine's cascade runs.
constexpr uintptr_t kGateCalls[] = { sdk::site::CCountry_InvalidateModifier_InvalidateChildren, sdk::site::CCountry_InvalidateRelayNetworkModifier_InvalidateChildren,
                                     sdk::site::CCountryRelayNetworkManager_UpdateRelayNetwork_InvalidateChildren };  // the InvalidateChildren calls held
constexpr int kGateSites = 3;
uintptr_t g_gate_sites[kGateSites];
bool g_gate_ok = false;
std::atomic<int> g_gate_mode{ 0 };
struct GateRow {
    std::atomic<uintptr_t> node;
    std::atomic<uint64_t> pre_hash;  // the content the children were built from (taken at the first held invalidation)
    std::atomic<uint32_t> pending;
};
constexpr int kGateSlots = 1024;
GateRow g_gate[kGateSlots];
std::atomic<int> g_gate_pending_n{ 0 };
std::atomic<uint64_t> g_gate_overflow{ 0 };
thread_local void* t_gate_node = nullptr;  // the node whose cascade was just held: rebuilt right after its AddInvalid
thread_local void* t_rb_any = nullptr;     // the node being rebuilt on this thread
std::atomic<uint64_t> g_gate_held{ 0 }, g_gate_worker{ 0 }, g_gate_released{ 0 }, g_gate_suppressed{ 0 }, g_gate_rebuild_cycles{ 0 };
std::atomic<uint64_t> g_gate_masked{ 0 }, g_gate_counted{ 0 }, g_gate_mismatch{ 0 };
std::atomic<uint64_t> g_gate_unhooked{ 0 };  // held invalidations run the engine's way because the node's delegate was not hooked yet
using NodeFn = uint64_t (*)(void*);

GateRow* FindGate(const void* node, bool create) {
    const uintptr_t key = (uintptr_t)node;
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    for (int i = 0; i < 16; ++i) {
        GateRow& g = g_gate[(h + (uint64_t)i) & (kGateSlots - 1)];
        uintptr_t k = g.node.load(std::memory_order_acquire);
        if (k == key) return &g;
        if (k == 0) {
            if (!create) return nullptr;
            uintptr_t expected = 0;
            if (g.node.compare_exchange_strong(expected, key)) return &g;
            if (expected == key) return &g;
        }
    }
    if (create) g_gate_overflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

bool GatePending(const void* node) {
    GateRow* g = FindGate(node, false);
    return g && g->pending.load(std::memory_order_relaxed);
}

inline bool IsGateSite(uintptr_t ret) {
    for (int i = 0; i < kGateSites; ++i)
        if (g_gate_sites[i] == ret) return true;
    return false;
}

// After a held node's rebuild: the cascade the engine would have done at the invalidation, when the content differs
void ResolveGate(void* node, uint64_t hash_now, bool have_hash) {
    GateRow* g = FindGate(node, false);
    if (!g || !g->pending.exchange(0, std::memory_order_acq_rel)) return;
    g_gate_pending_n.fetch_sub(1, std::memory_order_relaxed);
    const uint64_t h = have_hash ? hash_now : NodeHash(node);
    if (h != g->pre_hash.load(std::memory_order_relaxed)) {
        g_gate_released.fetch_add(1, std::memory_order_relaxed);
        if (g_gate_mode.load(std::memory_order_relaxed) != 3) g_orig_inv(node, 0xFFFFFFFFu);
    } else {
        g_gate_suppressed.fetch_add(1, std::memory_order_relaxed);
    }
}

// the invalidation in progress on this thread
thread_local int t_depth = 0;
thread_local uintptr_t t_root_site = 0, t_root_site2 = 0;
thread_local uint8_t t_root_cat = 0;
thread_local void* t_parent = nullptr;
// the rebuild in progress on this thread
thread_local void* t_pre_node = nullptr;
thread_local uint64_t t_pre_hash = 0;
thread_local uint64_t t_child_cycles = 0;

bool Monthly() {
    SharedStats* s = Shared();
    return s && s->monthly_active;
}

int Category(const void* node) {
    __try {
        const unsigned char* type = *(unsigned char* const*)node;
        const uint32_t c = *(const uint32_t*)(type + kTypeCategory);
        return c < (uint32_t)kMaxCat ? (int)c : kMaxCat - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kMaxCat - 1;
    }
}

uintptr_t DoCallOf(const void* node) {
    __try {
        const unsigned char* impl = *(unsigned char* const*)((const unsigned char*)node + kNodeCalc);
        if (!impl) return 0;
        const uintptr_t* vt = *(const uintptr_t* const*)impl;
        return vt[kCalcCallSlot];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* NodeOf(const void* mgr, uint32_t id) {
    __try {
        const unsigned char* m = (const unsigned char*)mgr;
        const uint32_t index = id & kIndexMask;
        if (index >= *(const uint32_t*)(m + sdk::rt::CModifierNodeManager_slot_count)) return nullptr;
        const unsigned char* slot = *(unsigned char* const*)(m + sdk::rt::CModifierNodeManager_slots) + index * kSlotStride;
        if (*(const uint32_t*)slot != id) return nullptr;
        return *(void* const*)(slot + sdk::rt::CModifierNodeManager_slot_node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Node content: the entries (value, type) and the parent records (mult, name, category) as sets: a rebuild lists
// the same entries in another order when the containers its calc walks were reshuffled, and the engine reads them by
// type, so the order is not content. Each record is hashed on its own and the hashes summed.
inline uint64_t Mix64(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return v;
}

// A parent record's source name is a CPdxModifierName object made anew at every rebuild (CPdxModifierNameRefCountedLocKeyImpl
// and the shared CPdxModifierName::_EmptyName): its content through the object's own CalcHash (a virtual of the name;
// the empty name's returns 0)
constexpr int kNameCalcHashSlot = sdk::vt::CPdxModifierName_CalcHash;
uint64_t NameHash(void* name) {
    if (!name) return 0;
    __try {
        const uintptr_t* vt = *(const uintptr_t* const*)name;
        const uintptr_t fn = vt[kNameCalcHashSlot];
        if (fn < g_text0 || fn >= g_text1) return (uint64_t)(uintptr_t)name;
        return ((uint32_t(*)(void*))fn)(name) + 1ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (uint64_t)(uintptr_t)name;
    }
}

uint64_t NodeHash(const void* node) {
    __try {
        const unsigned char* m = (const unsigned char*)node + sdk::rt::CModifierNode_modifier;
        const int n = *(const int32_t*)(m + sdk::rt::CModifier_entry_count);
        const unsigned char* e = *(unsigned char* const*)(m + sdk::rt::CModifier_entries);
        uint64_t h = Mix64((uint64_t)n * 0x9E3779B97F4A7C15ull);
        for (int i = 0; i < n; ++i)
            h += Mix64(*(const uint64_t*)(e + i * kEntrySize) * 0x9E3779B97F4A7C15ull + *(const uint32_t*)(e + i * kEntrySize + kEntryType));
        const int pn = *(const int32_t*)(m + sdk::rt::CModifier_parent_count);
        const unsigned char* pr = *(unsigned char* const*)(m + sdk::rt::CModifier_parents);
        h += Mix64((uint64_t)pn * 0xC2B2AE3D27D4EB4Full);
        for (int i = 0; i < pn; ++i) {
            uint64_t r = Mix64(*(const uint64_t*)(pr + i * kParentSize + kParentMult) + 0x1234567ull);
            r = Mix64(r ^ NameHash(*(void* const*)(pr + i * kParentSize + kParentName)));
            r = Mix64(r ^ *(const uint32_t*)(pr + i * kParentSize + kParentCat));
            h += r;
        }
        return h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool PlanetColonized(const void* planet) {
    __try {
        return *(const uint32_t*)((const unsigned char*)planet + kPlanetColony) != 0xFFFFFFFFu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Count(const InvKey& k) {
    const uint64_t a = (uint64_t)k.site | ((uint64_t)k.root_site << 32) | 1ull << 63;
    const uint64_t b = (uint64_t)k.cat | ((uint64_t)k.parent_cat << 8) | ((uint64_t)k.root_cat << 16) | ((uint64_t)k.kind << 24) |
                       ((uint64_t)k.root_site2 << 32);
    uint64_t h = (a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    for (int i = 0; i < 32; ++i) {
        InvRow& r = g_inv[(h + (uint64_t)i) & (kInvSlots - 1)];
        uint64_t ka = r.key_a.load(std::memory_order_acquire);
        if (ka == 0) {
            uint64_t expected = 0;
            if (r.key_a.compare_exchange_strong(expected, a)) {
                r.key_b.store(b, std::memory_order_release);
                r.n.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            ka = r.key_a.load(std::memory_order_acquire);
        }
        if (ka == a) {
            for (int s = 0; s < 100 && r.key_b.load(std::memory_order_acquire) == ~0ull; ++s) YieldProcessor();
            if (r.key_b.load(std::memory_order_acquire) == b) {
                r.n.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    g_inv_overflow.fetch_add(1, std::memory_order_relaxed);
}

NodeState* FindState(const void* node, bool create) {
    const uintptr_t key = (uintptr_t)node;
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    for (int i = 0; i < 32; ++i) {
        NodeState& f = g_nf[(h + (uint64_t)i) & (kNodeSlots - 1)];
        uintptr_t k = f.node.load(std::memory_order_acquire);
        if (k == key) return &f;
        if (k == 0) {
            if (!create) return nullptr;
            uintptr_t expected = 0;
            if (f.node.compare_exchange_strong(expected, key)) return &f;
            if (expected == key) return &f;
        }
    }
    if (create) g_nf_overflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// a cascade from parent to node: remembered with the moment, for the skip check at the node's rebuild
void NoteCascade(const void* node, const void* parent) {
    NodeState* e = FindState(node, true);
    if (!e) return;
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    e->flags.fetch_or(2u, std::memory_order_relaxed);
    const uint32_t n = e->nparents.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < (uint32_t)kMaxParents; ++i) {
        if (e->parent[i].load(std::memory_order_relaxed) == (uintptr_t)parent) {
            e->parent_seq[i].store(seq, std::memory_order_relaxed);  // the parent's content at the first cascade stays
            return;
        }
    }
    if (n >= (uint32_t)kMaxParents) {
        e->flags.fetch_or(4u, std::memory_order_relaxed);
        return;
    }
    // the parent is dirty but not rebuilt yet: its content is still what this node was last built from
    e->parent[n].store((uintptr_t)parent, std::memory_order_relaxed);
    e->parent_seq[n].store(seq, std::memory_order_relaxed);
    e->parent_hash[n].store(NodeHash(parent), std::memory_order_relaxed);
    e->nparents.store(n + 1, std::memory_order_relaxed);
}

// true when every parent that cascaded to the node has been rebuilt since and holds the content the node was last
// built from
bool ParentsOk(NodeState* e, uint32_t flags, uint32_t nparents) {
    if (!e || (flags & 4) || nparents == 0) return false;
    for (uint32_t i = 0; i < nparents && i < (uint32_t)kMaxParents; ++i) {
        const NodeState* p = FindState((const void*)e->parent[i].load(std::memory_order_relaxed), false);
        if (!p) return false;
        const uint32_t pseq = e->parent_seq[i].load(std::memory_order_relaxed);
        if (p->rebuild_seq.load(std::memory_order_relaxed) <= pseq) return false;
        if (p->last_hash.load(std::memory_order_relaxed) != e->parent_hash[i].load(std::memory_order_relaxed)) return false;
    }
    return true;
}

void ResetTables() {
    for (InvRow& r : g_inv) {
        r.n.store(0);
        r.key_b.store(~0ull);
        r.key_a.store(0);
    }
    g_inv_overflow.store(0);
    g_walks.store(0);
    for (auto& w : g_cat)
        for (auto& m : w)
            for (CatStat& c : m) {
                c.n = 0, c.cycles = 0, c.changed = 0, c.unchanged = 0, c.unknown = 0;
                c.direct = 0, c.cascade_only = 0, c.unflagged = 0;
                c.ok_unchanged = 0, c.ok_changed = 0, c.notok_unchanged = 0, c.notok_changed = 0;
            }
    for (int m = 0; m < 2; ++m) g_flush_n[m] = 0, g_flush_cycles[m] = 0;
    for (auto& p : g_pre_n) p.store(0);
    for (auto& d : g_diff_logged) d.store(0);
    for (auto& w : g_planet_rb)
        for (auto& m : w) m[0] = 0, m[1] = 0;
    for (auto& k : g_planet_inv)
        for (auto& m : k) m[0] = 0, m[1] = 0;
}

int Where() {
    if (g_in_flush.load(std::memory_order_relaxed)) return 2;
    return GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed) ? 0 : 1;
}

void InvalidateChildrenDetour(void* node, uint32_t mask) {
    const int gm = g_gate_mode.load(std::memory_order_relaxed);
    if (gm) {
        if (node == t_rb_any) {
            // a rebuild's own masked cascade of a held node: the gate decides from the content instead
            if (gm != 3 && GatePending(node)) {
                g_gate_masked.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else if (t_depth == 0 && mask == 0xFFFFFFFFu && IsGateSite((uintptr_t)_ReturnAddress())) {
            const bool main = GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed);
            if (main || gm >= 2) {
                if (!DelegateHooked(node)) {
                    // the node's rebuild delegate is hooked at the next day change (InstallDoCalls); until then the
                    // engine's cascade runs
                    LearnDoCall(node);
                    g_gate_unhooked.fetch_add(1, std::memory_order_relaxed);
                } else if (GateRow* g = FindGate(node, true)) {
                    if (!g->pending.exchange(1, std::memory_order_acq_rel)) {
                        g->pre_hash.store(NodeHash(node), std::memory_order_relaxed);
                        g_gate_pending_n.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (gm == 3) {
                        g_gate_counted.fetch_add(1, std::memory_order_relaxed);  // count only: the engine's cascade runs
                    } else {
                        g_gate_held.fetch_add(1, std::memory_order_relaxed);
                        t_gate_node = node;
                        return;
                    }
                }
            } else {
                g_gate_worker.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!g_trace.load(std::memory_order_relaxed)) return g_orig_inv(node, mask);
    const uintptr_t site = (uintptr_t)_ReturnAddress() - g_base;
    const int cat = Category(node);
    if (t_depth == 0) {
        t_root_site = 0;
        t_root_site2 = 0;
        t_root_cat = (uint8_t)cat;
        if (mask == 0xFFFFFFFFu && g_walks.fetch_add(1, std::memory_order_relaxed) < kMaxWalks) {
            // the eager invalidation (X::InvalidateModifier): who called it. frames[0] is this detour, [1] the site
            // in X::InvalidateModifier, [2] its caller, [3] the caller's caller
            void* frames[5] = {};
            const USHORT n = RtlCaptureStackBackTrace(0, 5, frames, nullptr);
            if (n > 2) t_root_site = (uintptr_t)frames[2] - g_base;
            if (n > 3) t_root_site2 = (uintptr_t)frames[3] - g_base;
        }
        if (mask != 0) {
            InvKey k{ (uint32_t)site, (uint32_t)t_root_site, (uint32_t)t_root_site2, (uint8_t)cat, 0xFF, 0xFF, (uint8_t)((Monthly() ? 1 : 0) | 2) };
            Count(k);
        }
    }
    void* const saved_parent = t_parent;
    t_parent = node;
    ++t_depth;
    g_orig_inv(node, mask);
    --t_depth;
    t_parent = saved_parent;
}

void LearnDoCall(const void* node) {
    if (Category(node) >= 35) return;  // the engine has 35 categories; anything else is not a node
    const uintptr_t fn = DoCallOf(node);
    if (fn < g_text0 || fn >= g_text1) return;
    const int n = g_docall_n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_docall[i].fn.load(std::memory_order_relaxed) == fn) return;
    // new: claim a slot
    for (;;) {
        const int k = g_docall_n.load(std::memory_order_acquire);
        if (k >= kMaxDoCall) {
            g_docall_overflow.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (int i = n; i < k; ++i)
            if (g_docall[i].fn.load(std::memory_order_relaxed) == fn) return;
        uintptr_t expected = 0;
        if (g_docall[k].fn.compare_exchange_strong(expected, fn)) {
            g_docall[k].cat.store(Category(node), std::memory_order_relaxed);
            g_docall_n.store(k + 1, std::memory_order_release);
            return;
        }
    }
}

// Whether the node's rebuild delegate is hooked (Rebuild resolves a held gate when the rebuild ends)
bool DelegateHooked(const void* node) {
    const uintptr_t fn = DoCallOf(node);
    if (!fn) return false;
    const int n = g_docall_n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_docall[i].fn.load(std::memory_order_relaxed) == fn) return g_docall[i].installed.load(std::memory_order_acquire);
    return false;
}

// The modifier belongs to a registered node (the Clear is the CPdxModifier's, called on stand-alone modifiers too):
// the node's id maps back to it in the manager's slot table
bool IsNode(const void* node) {
    void* mgr = g_mgr.load(std::memory_order_relaxed);
    if (!mgr) return false;
    __try {
        const uint32_t id = *(const uint32_t*)((const unsigned char*)node + kNodeId);
        return id != 0xFFFFFFFFu && NodeOf(mgr, id) == node;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every rebuild starts by clearing the node's modifier: the content before, for the change check, and the delegate
void ModifierClearDetour(void* modifier, void* out) {
    if (g_trace.load(std::memory_order_relaxed)) {
        void* node = (unsigned char*)modifier - sdk::rt::CModifierNode_modifier;
        if (IsNode(node)) {
            t_pre_node = node;
            t_pre_hash = NodeHash(node);
            LearnDoCall(node);
        }
    }
    g_orig_clear(modifier, out);
}

uint64_t Rebuild(int i, void* impl, void* node) {
    const DoCallFn orig = g_docall[i].orig;
    if (!g_trace.load(std::memory_order_relaxed)) {
        if (!g_gate_mode.load(std::memory_order_relaxed) && !g_gate_pending_n.load(std::memory_order_relaxed)) return orig(impl, node);
        void* const saved_any = t_rb_any;
        t_rb_any = node;
        const uint64_t r = orig(impl, node);
        t_rb_any = saved_any;
        ResolveGate(node, 0, false);
        return r;
    }
    const int cat = Category(node);
    const int where = Where();
    const int mo = Monthly() ? 1 : 0;
    if (!g_cat_calc[cat].load(std::memory_order_relaxed)) g_cat_calc[cat].store(CalcOf(impl), std::memory_order_relaxed);
    bool have_pre = t_pre_node == node;
    uint64_t h0 = t_pre_hash;
    t_pre_node = nullptr;
    NodeState* e = FindState(node, true);
    uint32_t flags = 0, nparents = 0;
    bool parents_ok = false;
    if (e) {
        flags = e->flags.exchange(0, std::memory_order_relaxed);
        nparents = e->nparents.load(std::memory_order_relaxed);
        parents_ok = ParentsOk(e, flags, nparents);
        e->nparents.store(0, std::memory_order_relaxed);
        // the flush clears the modifier inline, without the hooked Clear: the content from the invalidation then
        if (!have_pre && e->pre_valid.load(std::memory_order_relaxed)) {
            have_pre = true;
            h0 = e->pre_hash.load(std::memory_order_relaxed);
        }
        e->pre_valid.store(0, std::memory_order_relaxed);
    }
    const uint64_t saved_child = t_child_cycles;
    t_child_cycles = 0;
    void* const saved_any = t_rb_any;
    t_rb_any = node;
    const uint64_t t0 = __rdtsc();
    const uint64_t r = orig(impl, node);
    const uint64_t dt = __rdtsc() - t0;
    t_rb_any = saved_any;
    const uint64_t self = dt > t_child_cycles ? dt - t_child_cycles : 0;
    t_child_cycles = saved_child + dt;
    const uint64_t h1 = NodeHash(node);
    ResolveGate(node, h1, true);
    const bool changed = h1 != h0;
    if (e) {
        e->changed.store(have_pre ? (changed ? 2u : 1u) : 0u, std::memory_order_relaxed);
        e->last_hash.store(h1, std::memory_order_relaxed);
        e->rebuild_seq.store(g_seq.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
    }
    CatStat& s = g_cat[where][mo][cat];
    s.n.fetch_add(1, std::memory_order_relaxed);
    s.cycles.fetch_add(self, std::memory_order_relaxed);
    if (!have_pre) s.unknown.fetch_add(1, std::memory_order_relaxed);
    else (changed ? s.changed : s.unchanged).fetch_add(1, std::memory_order_relaxed);
    const uint32_t pre_copy = e ? e->pre_copy.exchange(0, std::memory_order_relaxed) : 0;
    if (flags & 1) s.direct.fetch_add(1, std::memory_order_relaxed);
    else if (flags & 2) {
        s.cascade_only.fetch_add(1, std::memory_order_relaxed);
        if (have_pre) {
            if (parents_ok) {
                (changed ? s.ok_changed : s.ok_unchanged).fetch_add(1, std::memory_order_relaxed);
                if (changed && pre_copy && g_diff_logged[cat].fetch_add(1, std::memory_order_relaxed) < kDiffPerCat)
                    LogDiff(cat, node, g_pre[cat][pre_copy - 1]);
            } else (changed ? s.notok_changed : s.notok_unchanged).fetch_add(1, std::memory_order_relaxed);
        }
    } else s.unflagged.fetch_add(1, std::memory_order_relaxed);
    return r;
}

template <size_t I>
uint64_t DoCallDetour(void* impl, void* node) {
    return Rebuild((int)I, impl, node);
}

template <size_t... I>
constexpr std::array<DoCallFn, sizeof...(I)> MakeStubs(std::index_sequence<I...>) {
    return { &DoCallDetour<I>... };
}
const std::array<DoCallFn, kMaxDoCall> kStubs = MakeStubs(std::make_index_sequence<kMaxDoCall>{});

// main thread, between ticks: hook the delegates discovered since
void InstallDoCalls() {
    const int n = g_docall_n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        DoCall& d = g_docall[i];
        if (d.installed.load(std::memory_order_relaxed)) continue;
        const uintptr_t fn = d.fn.load(std::memory_order_relaxed);
        if (!fn) continue;
        if (MH_CreateHook((LPVOID)fn, (LPVOID)kStubs[i], (LPVOID*)&d.orig) == MH_OK && MH_EnableHook((LPVOID)fn) == MH_OK) {
            d.installed.store(true, std::memory_order_release);
            Log("node_trace: hooked the rebuild delegate of category %d at RVA 0x%llX", d.cat.load(), (unsigned long long)(fn - g_base));
        } else {
            Log("node_trace: could not hook the rebuild delegate of category %d at RVA 0x%llX", d.cat.load(), (unsigned long long)(fn - g_base));
            d.installed.store(true, std::memory_order_release);  // do not retry
            d.fn.store(1, std::memory_order_relaxed);
        }
    }
}

void Dump();

}  // namespace

bool Install(uintptr_t base) {
    g_base = base;
    {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (memcmp(sec[i].Name, ".text", 6) == 0) {
                g_text0 = base + sec[i].VirtualAddress;
                g_text1 = g_text0 + sec[i].Misc.VirtualSize;
                break;
            }
        }
        if (!g_text0) {
            Log("nodes: no .text section found; node_trace unavailable");
            return false;
        }
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const uint64_t c0 = __rdtsc();
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    do {
        QueryPerformanceCounter(&q1);
    } while ((q1.QuadPart - q0.QuadPart) * 1000 < f.QuadPart * 20);  // 20 ms
    const uint64_t c1 = __rdtsc();
    g_ms_per_cycle = 20.0 / (double)(c1 - c0);
    ResetTables();
    const LPVOID inv = (LPVOID)(base + kRvaInvalidateChildren), clr = (LPVOID)(base + kRvaModifierClear);
    if (MH_CreateHook(inv, (LPVOID)&InvalidateChildrenDetour, (LPVOID*)&g_orig_inv) != MH_OK ||
        MH_CreateHook(clr, (LPVOID)&ModifierClearDetour, (LPVOID*)&g_orig_clear) != MH_OK || MH_EnableHook(inv) != MH_OK ||
        MH_EnableHook(clr) != MH_OK) {
        Log("nodes: could not hook; node_trace unavailable");
        return false;
    }
    g_installed = true;
    Log("nodes: hooked CModifierNodeBase::InvalidateChildren and the node modifier's Clear (node_trace)");
    g_defs_ok = true;  // the modifier definitions table (sdk::glob::CModifier_Definitions), for the diagnostics' type names
    // the cascade gate: its call sites (the delegates are learned and hooked as the nodes are first seen)
    bool gate = true;
    for (int i = 0; i < kGateSites; ++i) {
        const uint8_t* p = (const uint8_t*)(base + kGateCalls[i]);
        if (p[0] != 0xE8 || kGateCalls[i] + 5 + (uintptr_t)(int64_t)*(const int32_t*)(p + 1) != kRvaInvalidateChildren) {
            Log("nodes: no call to InvalidateChildren at RVA 0x%llX; modifier_cascade_gate unavailable", (unsigned long long)kGateCalls[i]);
            gate = false;
        }
        g_gate_sites[i] = base + kGateCalls[i] + 5;
    }
    g_gate_ok = gate;
    if (gate) Log("nodes: the country and relay node cascades gated at their 3 call sites (modifier_cascade_gate)");
    return true;
}

void SetGateMode(int mode) {
    if (!g_installed) return;
    if (mode < 0 || mode > 3 || !g_gate_ok) mode = 0;
    if (g_gate_mode.exchange(mode) != mode) Log("modifier_cascade_gate=%d", mode);
}

// perf_tweaks's AddInvalid detour, after the engine registered the node: a node whose cascade was just held is
// rebuilt now (the children's invalidation follows from the content, ResolveGate)
void AfterAddInvalid(void* mgr, uint32_t node_id) {
    if (!t_gate_node) return;
    void* node = t_gate_node;
    t_gate_node = nullptr;
    if (NodeOf(mgr, node_id) != node) {
        // not the held node's registration (never seen): the engine's cascade after all
        g_gate_mismatch.fetch_add(1, std::memory_order_relaxed);
        if (GateRow* g = FindGate(node, false))
            if (g->pending.exchange(0, std::memory_order_acq_rel)) g_gate_pending_n.fetch_sub(1, std::memory_order_relaxed);
        g_orig_inv(node, 0xFFFFFFFFu);
        return;
    }
    const uint64_t t0 = __rdtsc();
    ((NodeFn)(g_base + sdk::fn::CModifierNodeBase_Update))(node);
    g_gate_rebuild_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
}

void SetTrace(int mode) {
    if (!g_installed) return;
    if (mode < 0 || mode > 1) mode = 0;
    const int old = g_trace.exchange(mode);
    if (old == mode) return;
    Log("node_trace=%d", mode);
    if (mode) {
        g_day0 = 0;
        g_last_day = 0;
        ResetTables();
        for (NodeState& f : g_nf) {
            f.flags.store(0);
            f.nparents.store(0);
            f.rebuild_seq.store(0);
            f.changed.store(0);
            f.pre_valid.store(0);
            f.last_hash.store(0);
            f.pre_copy.store(0);
            f.node.store(0);
        }
        g_nf_overflow.store(0);
    } else {
        Dump();
    }
}

bool Tracing() { return g_trace.load(std::memory_order_relaxed) != 0; }
bool InFlush() { return g_in_flush.load(std::memory_order_relaxed); }

void TracedFlush(void* mgr, MgrFn orig) {
    g_mgr.store(mgr, std::memory_order_relaxed);
    g_main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const int mo = Monthly() ? 1 : 0;
    SharedStats* shared = Shared();
    if (shared) {
        shared->flush_tid = GetCurrentThreadId();
        InterlockedExchange((volatile LONG*)&shared->flush_active, 1);
    }
    g_in_flush.store(true, std::memory_order_relaxed);
    const uint64_t t0 = __rdtsc();
    orig(mgr);
    const uint64_t dt = __rdtsc() - t0;
    g_in_flush.store(false, std::memory_order_relaxed);
    if (shared) InterlockedExchange((volatile LONG*)&shared->flush_active, 0);
    g_flush_n[mo].fetch_add(1, std::memory_order_relaxed);
    g_flush_cycles[mo].fetch_add(dt, std::memory_order_relaxed);
}

void OnAddInvalid(void* mgr, uint32_t node_id, uint32_t category, uintptr_t return_address) {
    g_mgr.store(mgr, std::memory_order_relaxed);
    const uint32_t site = (uint32_t)(return_address - g_base);
    const bool cascade = site == (uint32_t)kCascadeSite;
    const int mo = Monthly() ? 1 : 0;
    InvKey k;
    k.site = site;
    k.cat = (uint8_t)(category < (uint32_t)kMaxCat ? category : kMaxCat - 1);
    k.kind = (uint8_t)mo;
    if (cascade) {
        k.parent_cat = t_parent ? (uint8_t)Category(t_parent) : 0xFE;
        k.root_site = (uint32_t)t_root_site;
        k.root_site2 = (uint32_t)t_root_site2;
        k.root_cat = t_root_cat;
    } else {
        k.parent_cat = 0xFF;
        k.root_site = 0;
        k.root_site2 = 0;
        k.root_cat = 0xFF;
    }
    Count(k);
    void* node = NodeOf(mgr, node_id);
    if (node) {
        if (NodeState* e = FindState(node, true)) {
            // the content before the rebuild this invalidation leads to: taken at the first invalidation since the
            // last rebuild (the engine's AddInvalid runs after this, the node is already marked dirty)
            if (!e->pre_valid.load(std::memory_order_relaxed)) {
                e->pre_hash.store(NodeHash(node), std::memory_order_relaxed);
                e->pre_valid.store(1, std::memory_order_relaxed);
                const int pc = (int)(category < (uint32_t)kMaxCat ? category : kMaxCat - 1);
                if (cascade && g_pre_n[pc].load(std::memory_order_relaxed) < kPreCopies) {
                    const int k = g_pre_n[pc].fetch_add(1, std::memory_order_relaxed);
                    if (k < kPreCopies && CopyEntries(node, &g_pre[pc][k])) e->pre_copy.store((uint32_t)k + 1, std::memory_order_relaxed);
                }
            }
        }
        if (cascade && t_parent) NoteCascade(node, t_parent);
        else if (NodeState* e = FindState(node, true)) {
            e->flags.fetch_or(cascade ? 2u : 1u, std::memory_order_relaxed);
            g_seq.fetch_add(1, std::memory_order_relaxed);
        }
        const int pcat = g_planet_cat.load(std::memory_order_relaxed);
        if (pcat >= 0 && (int)category == pcat) {
            const bool col = PlanetColonized((const unsigned char*)node + g_planet_off.load(std::memory_order_relaxed));
            g_planet_inv[cascade ? 1 : 0][mo][col ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OnPlanetCalc(const void* planet, const void* node) {
    const int mo = Monthly() ? 1 : 0;
    g_planet_rb[Where()][mo][PlanetColonized(planet) ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    if (g_planet_cat.load(std::memory_order_relaxed) >= 0) return;
    // learn the planet node's category and where the planet is relative to its node, checked through the manager's slot table
    void* mgr = g_mgr.load(std::memory_order_relaxed);
    if (!mgr) return;
    __try {
        const uint32_t id = *(const uint32_t*)((const unsigned char*)node + kNodeId);
        if (NodeOf(mgr, id) != node) {
            g_planet_learn_fail.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_planet_learn_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const ptrdiff_t off = (const unsigned char*)planet - (const unsigned char*)node;
    g_planet_off.store(off, std::memory_order_relaxed);
    g_planet_cat.store(Category(node), std::memory_order_release);
    Log("node_trace: the planet node is category %d, %lld bytes into CPlanet", Category(node), (long long)-off);
}

void OnDay(uint32_t day) {
    g_main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    InstallDoCalls();  // the delegates node_trace or the gate learned, hooked between ticks
    if (!g_trace.load(std::memory_order_relaxed)) return;
    if (g_day0 == 0) {
        g_day0 = day;
        g_last_day = day;
        return;
    }
    if (day == g_last_day) return;
    g_days += day > g_last_day ? day - g_last_day : 1;
    g_last_day = day;
    if (day - g_day0 >= kWindowDays) {
        Dump();
        ResetTables();
        g_day0 = day;
    }
}

namespace {

const char* WhereName(int w) { return w == 0 ? "demand_main" : w == 1 ? "demand_worker" : "flush"; }

void Dump() {
    const double ms = g_ms_per_cycle;
    const uint32_t days = g_last_day - g_day0;
    ++g_dumps;
    Log("node_trace: ---- window of %u days (day %u to %u) ----", days, g_day0, g_last_day);
    Log("node_trace: flush normal n=%llu wall_ms=%.1f | monthly n=%llu wall_ms=%.1f | inv_overflow=%llu nf_overflow=%llu walks=%u planet_learn_fail=%llu docall_overflow=%llu",
        (unsigned long long)g_flush_n[0].load(), (double)g_flush_cycles[0].load() * ms, (unsigned long long)g_flush_n[1].load(),
        (double)g_flush_cycles[1].load() * ms, (unsigned long long)g_inv_overflow.load(), (unsigned long long)g_nf_overflow.load(), g_walks.load(),
        (unsigned long long)g_planet_learn_fail.load(), (unsigned long long)g_docall_overflow.load());
    const int nd = g_docall_n.load();
    for (int i = 0; i < nd; ++i)
        Log("node_trace: delegate cat=%d rva=0x%llX %s", g_docall[i].cat.load(), (unsigned long long)(g_docall[i].fn.load() - g_base),
            g_docall[i].installed.load() ? "hooked" : "pending");
    for (int c = 0; c < kMaxCat; ++c) {
        const uintptr_t calc = g_cat_calc[c].load();
        if (calc) Log("node_trace: calc cat=%d rva=0x%llX", c, (unsigned long long)(calc >= g_base ? calc - g_base : calc));
    }
    for (int w = 0; w < 3; ++w) {
        for (int mo = 0; mo < 2; ++mo) {
            for (int c = 0; c < kMaxCat; ++c) {
                const CatStat& s = g_cat[w][mo][c];
                const uint64_t n = s.n.load();
                if (!n) continue;
                Log("node_trace: rebuild %s %s cat=%d n=%llu self_ms=%.2f changed=%llu unchanged=%llu unknown=%llu direct=%llu cascade_only=%llu"
                    " ok_unchanged=%llu ok_changed=%llu notok_unchanged=%llu notok_changed=%llu unflagged=%llu",
                    WhereName(w), mo ? "monthly" : "normal", c, (unsigned long long)n, (double)s.cycles.load() * ms, (unsigned long long)s.changed.load(),
                    (unsigned long long)s.unchanged.load(), (unsigned long long)s.unknown.load(), (unsigned long long)s.direct.load(),
                    (unsigned long long)s.cascade_only.load(), (unsigned long long)s.ok_unchanged.load(), (unsigned long long)s.ok_changed.load(),
                    (unsigned long long)s.notok_unchanged.load(), (unsigned long long)s.notok_changed.load(), (unsigned long long)s.unflagged.load());
            }
        }
    }
    for (int w = 0; w < 3; ++w)
        for (int mo = 0; mo < 2; ++mo)
            if (g_planet_rb[w][mo][0].load() || g_planet_rb[w][mo][1].load())
                Log("node_trace: planet rebuild %s %s colonized=%llu uncolonized=%llu", WhereName(w), mo ? "monthly" : "normal",
                    (unsigned long long)g_planet_rb[w][mo][1].load(), (unsigned long long)g_planet_rb[w][mo][0].load());
    for (int k = 0; k < 2; ++k)
        for (int mo = 0; mo < 2; ++mo)
            if (g_planet_inv[k][mo][0].load() || g_planet_inv[k][mo][1].load())
                Log("node_trace: planet invalid %s %s colonized=%llu uncolonized=%llu", k ? "cascade" : "direct", mo ? "monthly" : "normal",
                    (unsigned long long)g_planet_inv[k][mo][1].load(), (unsigned long long)g_planet_inv[k][mo][0].load());
    struct Row { InvKey k; uint64_t n; };
    std::vector<Row> rows;
    for (const InvRow& r : g_inv) {
        const uint64_t a = r.key_a.load(), b = r.key_b.load(), n = r.n.load();
        if (!(a & 1ull << 63) || b == ~0ull || !n) continue;
        Row row;
        row.k.site = (uint32_t)a;
        row.k.root_site = (uint32_t)(a >> 32) & 0x7FFFFFFFu;
        row.k.cat = (uint8_t)b;
        row.k.parent_cat = (uint8_t)(b >> 8);
        row.k.root_cat = (uint8_t)(b >> 16);
        row.k.kind = (uint8_t)(b >> 24);
        row.k.root_site2 = (uint32_t)(b >> 32);
        row.n = n;
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.n > y.n; });
    int direct = 0, cascade = 0, roots = 0;
    for (const Row& r : rows) {
        const char* kind = (r.k.kind & 1) ? "monthly" : "normal";
        if (r.k.kind & 2) {
            if (roots++ >= 60) continue;
            Log("node_trace: root site=0x%X cat=%u caller=0x%X caller2=0x%X %s n=%llu", r.k.site, r.k.cat, r.k.root_site, r.k.root_site2, kind,
                (unsigned long long)r.n);
        } else if (r.k.parent_cat == 0xFF) {
            if (direct++ >= 60) continue;
            Log("node_trace: direct site=0x%X cat=%u %s n=%llu", r.k.site, r.k.cat, kind, (unsigned long long)r.n);
        } else {
            if (cascade++ >= 100) continue;
            Log("node_trace: cascade cat=%u parent_cat=%u root_cat=%u caller=0x%X caller2=0x%X %s n=%llu", r.k.cat, r.k.parent_cat, r.k.root_cat,
                r.k.root_site, r.k.root_site2, kind, (unsigned long long)r.n);
        }
    }
    Log("node_trace: ---- end (%zu rows: %d direct, %d cascade, %d root) ----", rows.size(), direct, cascade, roots);
}

}  // namespace

std::string Stats() {
    if (!g_installed) return "modifier_cascade_gate=unavailable | node_trace=unavailable";
    char buf[360];
    snprintf(buf, sizeof buf,
             "modifier_cascade_gate=%d(held=%llu worker_passed=%llu released=%llu suppressed=%llu rebuild_ms=%.1f masked=%llu counted=%llu mismatch=%llu"
             " unhooked=%llu) | node_trace=%d(days=%llu dumps=%llu)",
             g_gate_ok ? g_gate_mode.load() : -1, (unsigned long long)g_gate_held.load(), (unsigned long long)g_gate_worker.load(),
             (unsigned long long)g_gate_released.load(), (unsigned long long)g_gate_suppressed.load(), (double)g_gate_rebuild_cycles.load() * g_ms_per_cycle,
             (unsigned long long)g_gate_masked.load(), (unsigned long long)g_gate_counted.load(), (unsigned long long)g_gate_mismatch.load(),
             (unsigned long long)g_gate_unhooked.load(), g_trace.load(), (unsigned long long)g_days, (unsigned long long)g_dumps);
    return buf;
}

}  // namespace perf::nodes
