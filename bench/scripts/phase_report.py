"""phase_report.py [phases.json] [--last N] [--all]: the plugin's phase timing (phase_profile=1) as a table.

Reads logs\\stellaris_perf_phases.json of the plugin folder (or the file given): one record per game day with the wall
time and the exclusive time of every phase (see perf/include/perf_phase.hpp). Days are grouped by kind: a normal day,
the first of a month, 1 January and 1 July (the half-yearly autosave). Incomplete days (not 10 ticks) and outliers
(wall time above 3x the group's median: a pause, a window being opened) are left out unless --all is given.
Prints, per group, the mean ms per game day of every phase and its share of the wall time.
"""
import argparse
import json
import os
import statistics
import sys

from benchlib import PERF_LOGS

GROUPS = [("normal day", lambda doy: doy % 30 != 0),
          ("1st of month (not Jan/Jul)", lambda doy: doy % 30 == 0 and doy not in (0, 180)),
          ("1 January", lambda doy: doy == 0),
          ("1 July", lambda doy: doy == 180)]
# how the phases roll up (the save runs between ticks, from the frame function)
TICK = ("tick", "micro", "daily", "monthly", "yearly", "ai")
SAVE = ("save",)
RENDER = ("render_in_tick", "render_between")
UI = ("frame", "gui", "gui2", "app", "draw")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", default=os.path.join(PERF_LOGS, "stellaris_perf_phases.json"))
    ap.add_argument("--last", type=int, default=0, help="only the last N days")
    ap.add_argument("--all", action="store_true", help="keep incomplete days and outliers")
    a = ap.parse_args()
    J = json.load(open(a.file, encoding="utf-8"))
    names = J["names"]
    ms = 1000.0 / J["tsc_per_sec"]
    days = J["days"]
    if a.last:
        days = days[-a.last:]
    if J.get("dropped"):
        print(f"note: {J['dropped']} days were not recorded (table full)")
    print(f"{len(days)} days recorded")
    for title, pred in GROUPS:
        rows = [d for d in days if pred(d[1])]
        if not a.all:
            rows = [d for d in rows if d[2] == 10]
            if len(rows) >= 3:
                med = statistics.median(d[3] for d in rows)
                rows = [d for d in rows if d[3] <= 3 * med]
        if not rows:
            print(f"\n== {title}: no days")
            continue
        n = len(rows)
        wall = [d[3] * ms for d in rows]
        mean_wall = sum(wall) / n
        print(f"\n== {title}: {n} days, wall {mean_wall:.1f} ms/day (median {statistics.median(wall):.1f}, min {min(wall):.1f}, max {max(wall):.1f})")
        k = len(names)
        ph = {nm: sum(d[4 + i] for d in rows) * ms / n for i, nm in enumerate(names)}
        calls = {nm: (sum(d[4 + k + i] for d in rows) / n if len(rows[0]) >= 4 + 2 * k else None) for i, nm in enumerate(names)}
        acc = sum(ph.values())
        groups = [("tick total", TICK), ("save (between ticks)", SAVE), ("render", RENDER), ("ui per frame", UI)]
        for gname, members in groups:
            tot = sum(ph[m] for m in members)
            print(f"  {gname:<16} {tot:8.1f} ms/day {100 * tot / mean_wall:5.1f}%")
            for m in members:
                c = calls[m]
                extra = f"   {c:7.1f} calls/day, {ph[m] / c * 1000:7.0f} us each" if c else ""
                print(f"    {m:<16} {ph[m]:8.1f} ms/day {100 * ph[m] / mean_wall:5.1f}%{extra}")
        print(f"  {'unaccounted':<16} {mean_wall - acc:8.1f} ms/day {100 * (mean_wall - acc) / mean_wall:5.1f}%  (main loop outside the frame function, waits between ticks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
