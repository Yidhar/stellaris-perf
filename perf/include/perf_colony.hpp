#pragma once

#include <cstdint>
#include <string>

namespace perf::colony {

// colony_monthly_batch: less modifier work inside a colony's monthly pass.
//
// The colony monthly pass (CColony::MonthlyUpdateSerial: migration, growth, assembly, decline, each applying amounts
// the parallel demography pass computed beforehand) changes pop-group sizes one group at a time. Every change
// invalidates the colony's modifier node (and its children), and the node is rebuilt on demand by the next reader:
// the triggers of the on_action events the change fires, the forced refresh in CColony::OnAddPops / OnRemovePops
// (CModifierNodeBase::Update, then CColony::MicroUpdateCachedValues for amenities, crime and stability, then another
// invalidation when they changed), a new pop group's refresh. A late save rebuilds the node about 1,300 times per
// month start this way, and the modifier manager's flush runs a few times inside the passes.
//
// 1: the forced refreshes are skipped for the colony whose pass is running and done once when it ends, and the
//    manager's flush is held while a pass runs (its dirty nodes are rebuilt by the next flush after the pass). Readers
//    rebuild a dirty node on demand, so nothing a script or the engine reads changes.
// 2: in addition the colony's own node is frozen during its pass: every read of it from inside the pass (event
//    triggers, the pass's own code) gets the node as it was when the pass began; it is rebuilt once at the end and the
//    colony invalidated again, so what was computed from the frozen node is rebuilt afterwards. Deterministic (a
//    function of the game state, main thread only): two clients that both run it agree.
// What differs from the engine in both: a colony whose amenities or housing tier moves during its own pass gets the
// stability of the tier from before the pass (the engine: from before the pass's last change); an on_pop_growth /
// on_pop_purged event reading the cached values sees the pre-pass ones. 0 = off.
bool Install(uintptr_t base);
void SetMode(int mode);
// True while a colony's pass runs with the option on: the modifier manager's flush (perf_tweaks's detour of
// CModifierNodeManager::Update asks) is skipped, its dirty nodes stay for the next flush after the pass.
bool HoldFlush();
bool Installed();
std::string Stats();

// pop_jobs_node_batch: the two modifier refreshes the monthly pop-job pass forces for every pop group its promotions
// create (the source group's node and the new group's, each rebuilding the colony's and the planet's node first,
// dirty again at every one of them) skipped inside the pass. 2 = the nodes left dirty for their next reader, like any
// node the engine invalidates (the default; half of them are never read again before the next pass); 1 = the nodes
// rebuilt once each when the pass ends (the same values, from the pass's final state; no gain, the colony and planet
// rebuilds repeat); 0 = the engine's refreshes, counted; 3 = 2 with the nodes watched for who rebuilds them (stats).
// perf_jobs opens the pass around every UpdatePopJobs call (main thread).
void SetJobsMode(int mode);
void BeginJobsPass();
void EndJobsPass();
// pop_jobs_flush_first: the loop at the start of each pass that refreshes every colony's node (serial) is timed, and
// skipped when asked (the pass's readers rebuild the nodes on demand). False when the loop's call was not found.
bool SetJobsColonySkip(bool skip);
void JobsColonyLoopStats(uint64_t* calls, uint64_t* us, uint64_t* skipped);

}  // namespace perf::colony
