#pragma once

#include <cstdint>
#include <string>

namespace perf::nodes {

// node_trace (measurement only, off): where the modifier graph's work comes from, per 30 game days, written to the
// log as `node_trace:` lines.
//  * every invalidation (CModifierNodeManager::AddInvalid, the tail of every X::InvalidateModifier and of the cascade
//    to a node's children) by call site, node category, and for a cascade the parent's category and the root site
//    and category that started it (CModifierNodeBase::InvalidateChildren is hooked for that);
//  * every rebuild of a node (the calc delegate each node type holds as a std::function, found from the nodes and
//    hooked between ticks; the node modifier's Clear, the first step of every rebuild, is hooked for the content
//    before) by category, split into the manager's flush, on demand on the main thread, on demand on a worker, each
//    on normal days and inside the monthly update; self time (nested parent rebuilds taken out); whether the rebuild
//    changed the node's modifier (a hash of its entries and parents before and after); and whether the node had been
//    invalidated directly since its last rebuild or only by a cascade from a parent;
//  * the flush itself: count and wall time;
//  * the planet rebuilds and invalidations split by colonized / not.
//
// modifier_cascade_gate: the country's modifier node is invalidated several times a day (its daily update when a
// faction's support or approval moved, the relay network update when the network's systems changed, a planet's
// daily update, the daily pop-job recompute, leader and starbase changes), and every invalidation marks all of its
// children dirty at once (CModifierNodeBase::InvalidateChildren, every category): the planets (colonized or not),
// pop groups, fleets, ships, armies, starbases, leaders of the country, about 7,000 nodes a day on a late save, all
// rebuilt by the modifier manager's next flush (15 ms wall a day), while the country's rebuilt content is identical to
// the previous one nine times out of ten (the children's rebuilds are then no-ops). The same for the relay network
// node, a parent of every planet. With the gate the cascade from the country's main node and from the relay node is
// held at its three call sites: the node is rebuilt at once on the invalidating thread (the engine's own daily path
// rebuilds it right there anyway) and its children are invalidated the engine's way only when the rebuilt content
// differs from what they were built from (entries by type and the parent records' multiplier, category and source
// name content, as sets). 2 = every invalidation, the default (the relay network update runs in a parallel task: the
// rebuild then runs inside it, as the engine's own on-demand rebuild of a dirty node read by a task does); 1 = on the
// main thread only (that task keeps the engine's cascade); 3 = count only (the engine's cascades, with the content
// compared). Measured on a late save: 11 held invalidations a day, 93 % with the content unchanged; the daily flush
// drops from 10,100 to 3,500 rebuilds and from 15 to 7.5 ms of wall time a day. What differs from the engine: a
// child whose own inputs changed without an invalidation of its own (the engine refreshes it through the next
// country-wide cascade) stays as it was until its next invalidation or the next change of the country's content:
// about seven nodes a day on the late save, colonies and planets mostly.
bool Install(uintptr_t base);
void SetGateMode(int mode);
void AfterAddInvalid(void* mgr, uint32_t node_id);
void SetTrace(int mode);
bool Tracing();
bool InFlush();
using MgrFn = void (*)(void*);
void TracedFlush(void* mgr, MgrFn orig);
void OnAddInvalid(void* mgr, uint32_t node_id, uint32_t category, uintptr_t return_address);
void OnPlanetCalc(const void* planet, const void* node);
void OnDay(uint32_t day);
std::string Stats();

}  // namespace perf::nodes
