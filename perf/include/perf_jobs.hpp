#pragma once

#include <cstdint>
#include <string>

namespace perf::jobs {

// pop_jobs_single_pass: the monthly pop-job assignment once instead of twice.
//
// NColonyUpdate::MonthlyUpdate calls NPlanetJobs::UpdatePopJobs(date, all colonies) twice in a row. The second call
// is one more fixed-point iteration over what the first changed: pop groups the first collapsed or promoted keep
// the jobs they were given, the modifiers the first invalidated (job checksums changing the country's economy) are
// rebuilt and can move job slots and workforce, and the few job weights that read the assignment (the zoo animals'
// num_assigned_jobs, healthcare's free_amenities) see the new one. The engine's own daily recompute of a colony whose
// buildings changed is a single pass. With this on the second monthly call is skipped (and the second of the pair in
// the cheat-mode daily variant): the month's assignment is the first pass's, and the next recompute picks up what the
// second would have changed. Deterministic (a function of the game state, main thread): two clients that both run it
// agree.
//
// pop_faction_batch: inside a window, CPopFaction::UpdateSupportAndApproval (which the engine runs for every faction
// a pop group belongs to each time pops move between groups: CalcSupport, then CalcApproval over the faction type's
// scripted demands, then the member groups invalidated when the approval changed) is run once per faction when the
// window ends instead of once per move. Nothing in the window reads a faction's support or approval (the promotion
// loop decides by precomputed category flags; growth, migration and the job scripts do not read factions), so the
// values are those of the same final state; a faction whose values changed and changed back inside the window is
// not invalidated or flagged then, and a script that did read them would see the values from before the window.
// 0 = the engine's calls (counted inside the passes); 1 = the window is each pass of UpdatePopJobs; 2 = the window is
// the whole NColonyUpdate::MonthlyUpdate (growth, migration, decline, assembly, the passes), the default; 3 / 4 =
// verify: the engine's calls, then at the end of the window of 1 / 2 every faction touched recomputed once more from
// the final state and compared (mismatches logged and counted).
bool Install(uintptr_t base);
void SetEnabled(bool on);
void SetFactionMode(int mode);
bool Installed();
std::string Stats();

}  // namespace perf::jobs
