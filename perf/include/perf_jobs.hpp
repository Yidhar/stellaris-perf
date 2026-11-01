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
bool Install(uintptr_t base);
void SetEnabled(bool on);
bool Installed();
std::string Stats();

}  // namespace perf::jobs
