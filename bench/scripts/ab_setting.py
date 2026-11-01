"""A/B wall-clock test of one plugin setting in a running game (ABBA / BAAB groups in one continuous run).

    python ab_setting.py <groups> <days per segment> <out.json> --a alloc_cache=0 --b alloc_cache=1 [--first 0|1] [--warm 5]
                         [--base key=value ...]

The plugin must be loaded and the game paused with a save loaded (game_session.py). `--base` sets the keys both arms share
(default: the plugin's own defaults; every key not named here keeps its default). Each segment switches the ini, waits until
the plugin applied it, lets the game run one day to settle, then times the next <days> days: wall milliseconds per game day
(days with a pause, such as an event pop-up, are left out) and process CPU milliseconds per game day. Groups are ABBA
and BAAB alternately, so a drift in the game's speed cancels; the paired difference per group is reported with a 95 %
confidence interval.
"""
import argparse
import ctypes
import ctypes.wintypes as w
import json
import math
import statistics
import sys
import time

import benchlib
from benchlib import Bench, PerfStats, game_pid

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE

KEYS = ("frame_smoothing", "opinion_cache", "rule_cache", "fleet_manager_cache", "fleet_manager_reinforce_ms", "flag_simd",
        "flag_expiry_skip", "profile", "rule_profile", "modifier_flush", "modifier_flush_max", "fleet_parallel_grain1",
        "scope_profile", "alloc_cache", "alloc_cache_mb", "combat_no_copy", "colony_monthly_batch", "pop_jobs_single_pass",
        "ai_parallel_lpt", "ai_weight_cache", "ai_nested_inline", "tick_render_fps", "ui_probe", "render_cap_between_ticks",
        "phase_profile")
DEFAULTS = {"frame_smoothing": -1, "opinion_cache": 0, "rule_cache": 0, "fleet_manager_cache": 1,
            "fleet_manager_reinforce_ms": 1000, "flag_simd": 1, "flag_expiry_skip": 1, "profile": 0, "rule_profile": 0,
            "modifier_flush": 0, "modifier_flush_max": 32, "fleet_parallel_grain1": 1, "scope_profile": 0, "alloc_cache": 1,
            "alloc_cache_mb": 8, "combat_no_copy": 1, "colony_monthly_batch": 2, "pop_jobs_single_pass": 1, "ai_parallel_lpt": 0,
            "ai_weight_cache": 1, "ai_nested_inline": 0, "tick_render_fps": 10,
            "ui_probe": 0, "render_cap_between_ticks": 1, "phase_profile": 0}


def parse(pairs):
    out = {}
    for p in pairs:
        k, v = p.split("=", 1)
        if k not in KEYS:
            raise SystemExit(f"unknown setting {k}")
        out[k] = int(v)
    return out


def cpu_seconds(h):
    c, e, kt, ut = (w.FILETIME() for _ in range(4))
    k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    ft = lambda x: ((x.dwHighDateTime << 32) | x.dwLowDateTime) / 1e7
    return ft(kt) + ft(ut)


def tcrit(df):
    return {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23}.get(df, 2.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("groups", type=int)
    ap.add_argument("days", type=int)
    ap.add_argument("out")
    ap.add_argument("--a", nargs="+", required=True, help="settings of arm A, key=value")
    ap.add_argument("--b", nargs="+", required=True, help="settings of arm B, key=value")
    ap.add_argument("--base", nargs="*", default=[], help="settings both arms share")
    ap.add_argument("--first", type=int, default=0, help="0: start with an ABBA group, 1: with BAAB")
    ap.add_argument("--warm", type=int, default=5, help="game days before the first segment")
    args = ap.parse_args()

    base = dict(DEFAULTS)
    base.update(parse(args.base))
    arms = {"A": dict(base, **parse(args.a)), "B": dict(base, **parse(args.b))}

    pid = game_pid()
    bench = Bench()
    stats = PerfStats(pid)
    proc = k32.OpenProcess(0x1000 | 0x0400, False, pid)

    current = [None]  # the arm in effect; the plugin only reports an apply when the settings changed
    seq_first = [True]

    def apply(arm):
        if current[0] == arm:
            return
        current[0] = arm
        seq = stats.read()["applied_seq"]
        benchlib.write_perf_ini(**arms[arm])
        first_call = seq_first[0]
        seq_first[0] = False
        t = time.time()
        while stats.read()["applied_seq"] == seq:
            if time.time() - t > 15:
                if first_call:  # the first arm may equal what the plugin already runs: nothing to apply
                    return
                raise SystemExit("the plugin did not apply the settings (is it loaded?)")
            time.sleep(0.1)

    def advance(days, timed):
        d0 = bench.status()["day"]
        target = d0 + days
        bench.cmd("pause 0")
        marks, stalled = [], set()
        last, t0, cpu0 = d0, time.time(), cpu_seconds(proc)
        while True:
            st = bench.status()
            now = time.time()
            if st["paused"]:
                bench.cmd("pause 0")
                stalled.add(st["day"])
            if st["day"] != last:
                marks.append((st["day"], now, last in stalled or st["day"] in stalled, cpu_seconds(proc)))
                last = st["day"]
            if st["day"] >= target or now - t0 > 600:
                break
            time.sleep(0.1)
        bench.cmd("pause 1")
        if not timed or len(marks) < 2:
            return None
        gaps = [(tb - ta) / (db - da) for (da, ta, _, _), (db, tb, sb, _) in zip(marks, marks[1:]) if not sb and db > da]
        span = max(marks[-1][0] - marks[0][0], 1)
        return (statistics.mean(gaps) * 1000 if gaps else None), (marks[-1][3] - marks[0][3]) / span * 1000, len(gaps)

    apply("A")
    advance(args.warm, False)
    seq = []
    for i in range(args.groups):
        seq += list("ABBA" if (i + args.first) % 2 == 0 else "BAAB")
    segs = []
    for k, arm in enumerate(seq):
        apply(arm)
        advance(1, False)
        r = advance(args.days, True)
        if r is None or r[0] is None:
            print(f"  seg {k:2d} {arm}: not enough clean days")
            continue
        segs.append({"i": k, "arm": arm, "ms_day": r[0], "cpu_ms_day": r[1], "clean_gaps": r[2]})
        print(f"  seg {k:2d} {arm}: {r[0]:7.1f} ms/day wall   {r[1]:8.0f} ms/day CPU   ({r[2]} clean day gaps)")
    final = stats.read()
    json.dump({"a": arms["A"], "b": arms["B"], "segments": segs, "stats": final}, open(args.out, "w"))

    for key, label in (("ms_day", "wall ms/day"), ("cpu_ms_day", "CPU ms/day")):
        diffs = []
        for g in range(0, len(segs) - 3, 4):
            grp = segs[g:g + 4]
            a = [s[key] for s in grp if s["arm"] == "A"]
            b = [s[key] for s in grp if s["arm"] == "B"]
            if len(a) == 2 and len(b) == 2:
                diffs.append((statistics.mean(b) / statistics.mean(a) - 1) * 100)
        A = statistics.mean(s[key] for s in segs if s["arm"] == "A")
        B = statistics.mean(s[key] for s in segs if s["arm"] == "B")
        msg = f"{label}: A {A:.1f}  B {B:.1f}  ({(B / A - 1) * 100:+.1f}%)"
        if len(diffs) >= 2:
            m = statistics.mean(diffs)
            se = statistics.stdev(diffs) / math.sqrt(len(diffs))
            msg += f"   paired per group: {m:+.1f}% +/- {tcrit(len(diffs) - 1) * se:.1f}% (95% CI, {len(diffs)} groups): " + " ".join(f"{d:+.1f}" for d in diffs)
        print(msg)
    print("plugin counters:", {k: v for k, v in final.items() if k.startswith("alloc_")})
    return 0


if __name__ == "__main__":
    sys.exit(main())
