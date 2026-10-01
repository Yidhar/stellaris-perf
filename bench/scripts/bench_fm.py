"""Automated A/B of stellaris_perf.dll settings in one continuous run (no save reloads).

    python bench_fm.py [--blocks 4] [--days 60] [--settle 3] [--smooth 1] [--no-fm-check]
                       [--config NAME:key=val,key=val ...]

Needs stellaris_perf.dll and stellaris_bench.dll loaded (dllctl.py load all) and a game in
progress. Settings switch while the game runs; each segment lets `settle` days pass, then times
`days` days. Segments run in ABBA / BAAB blocks, so a steady drift (the game slowing down as it goes on)
cancels out of the paired difference.

A segment counts only if
  * the settings it asked for show up in stellaris_perf.dll's statistics before timing starts;
  * with the fleet manager check on (default), CFleetManagerView::Update ran at least 0.9 times per
    rendered frame (it can run more than once per Present): some event popups hide the window and
    it stops updating. The script then pauses, asks
    for the window to be reopened, waits until it updates again and repeats the segment.
Event popups also pause the game: the script unpauses it and leaves out every gap of more than 2 s
between two days.

Defaults compare the fleet manager fixes (frame smoothing on, the game's default):
  base  fleet_manager_cache=0, fleet_manager_reinforce_ms=0
  fix   fleet_manager_cache=1, fleet_manager_reinforce_ms=1000
Results go to bench/results/.
"""
import argparse
import json
import math
import os
import statistics
import sys
import time

from benchlib import Bench, PerfStats, game_pid, module_loaded, write_perf_ini

RESULTS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "results"))
SPEED = 4  # the speed the earlier manual runs used
GAP = 60.0  # seconds; a longer gap between two days is a hang, not simulation (pauses are detected directly)
T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23,
       12: 2.18, 15: 2.13, 20: 2.09, 30: 2.04}


def t95(df):
    return T95.get(df) or T95[min((k for k in T95 if k >= df), default=30)]


def parse_config(text):
    name, _, body = text.partition(":")
    cfg = {}
    for kv in filter(None, body.split(",")):
        k, _, v = kv.partition("=")
        cfg[k.strip()] = int(v)
    return name, cfg


class Runner:
    def __init__(self, args):
        self.args = args
        self.pid = game_pid()
        for dll in ("stellaris_perf.dll", "stellaris_bench.dll"):
            if not module_loaded(self.pid, dll):
                raise SystemExit(f"{dll} is not loaded: python dllctl.py load all")
        self.bench = Bench()
        self.perf = PerfStats(self.pid)
        if not self.bench.status()["in_game"]:
            raise SystemExit("no game in progress")

    def ensure_running(self):
        st = self.bench.status()
        if st["speed"] != SPEED:
            st = self.bench.cmd(f"speed {SPEED}")
        if st["paused"]:
            st = self.bench.cmd("pause 0")
        return st

    def apply(self, cfg):
        write_perf_ini(frame_smoothing=self.args.smooth, **cfg)
        deadline = time.time() + 6
        while time.time() < deadline:
            # the DLL re-reads its ini every 2 s and publishes what it applied
            p = self.perf.read()
            if p["frame_smoothing"] == self.args.smooth and all(p[k] == v for k, v in cfg.items()):
                return
            time.sleep(0.1)
        raise SystemExit(f"stellaris_perf.dll did not apply {cfg} (is its ini polling running?)")

    def fm_visible(self, seconds=1.0):
        """Share of frames on which the fleet manager updated, over `seconds`."""
        s0, p0 = self.bench.status(), self.perf.read()
        time.sleep(seconds)
        s1, p1 = self.bench.status(), self.perf.read()
        frames = s1["frames"] - s0["frames"]
        return (p1["fleet_manager_updates"] - p0["fleet_manager_updates"]) / frames if frames else 0.0

    def wait_for_window(self):
        self.bench.cmd("pause 1")
        print("  !! 舰队管理器没有在更新（可能被事件弹窗隐藏了）。请关掉弹窗、重新打开舰队管理器并选中同一个模板，"
              "脚本检测到窗口恢复后会自动继续。", flush=True)
        while self.fm_visible(1.0) < 0.9:
            pass
        print("  窗口已恢复，继续", flush=True)

    def wait_days(self, n):
        """Lets n days pass; returns (simulated seconds, days, stalls, frames, wall seconds, fm updates)."""
        st = self.ensure_running()
        # align on a day boundary
        d = st["day"]
        while st["day"] == d:
            time.sleep(0.01)
            st = self.bench.status()
        start, last_day, last_t = st, st["day"], st["t"]
        p0 = self.perf.read()
        sim, days, stalls, kick, paused_seen = 0.0, 0, 0, 0.0, False
        while last_day < start["day"] + n:
            time.sleep(0.01)
            st = self.bench.status()
            if st["day"] != last_day:
                # an interval counts unless the game was seen paused in it (a slow day is still a day)
                if not paused_seen and st["t"] - last_t < GAP:
                    sim += st["t"] - last_t
                    days += st["day"] - last_day
                else:
                    stalls += 1
                last_day, last_t, paused_seen = st["day"], st["t"], False
            elif st["paused"]:
                paused_seen = True
                if time.time() - kick > 1.0:
                    self.bench.cmd("pause 0")  # an event popup paused the game
                    kick = time.time()
        p1 = self.perf.read()
        return (sim, days, stalls, st["frames"] - start["frames"], st["t"] - start["t"],
                p1["fleet_manager_updates"] - p0["fleet_manager_updates"])

    def segment(self, name, cfg):
        while True:
            self.apply(cfg)
            self.wait_days(self.args.settle)
            sim, days, stalls, frames, wall, fm = self.wait_days(self.args.days)
            fm_share = fm / frames if frames else 0.0
            rec = {"config": name, "ms_per_day": sim * 1000 / max(days, 1), "days": days, "stalls": stalls,
                   "fps": frames / wall if wall else 0.0, "fm_share": fm_share, "time": time.strftime("%H:%M:%S")}
            ok = self.args.no_fm_check or fm_share >= 0.9
            print(f"  {rec['time']} {name:6s} {rec['ms_per_day']:7.1f} ms/day  {rec['fps']:5.1f} fps  "
                  f"days={days} stalls={stalls} fm_updates/frame={fm_share:4.2f}{'' if ok else '  INVALID'}", flush=True)
            if ok:
                return rec
            self.wait_for_window()


def summarize(configs, records, blocks):
    """Means are dragged around by the odd heavy segment (a game event can double a segment's cost),
    so the medians and the sign count of the paired differences are printed next to them."""
    names = [n for n, _ in configs]
    out = {"per_config": {}, "paired": None}
    print("\n--- per config")
    for n in names:
        ms = [r["ms_per_day"] for r in records if r["config"] == n]
        fps = [r["fps"] for r in records if r["config"] == n]
        sd = statistics.stdev(ms) if len(ms) > 1 else 0.0
        out["per_config"][n] = {"n": len(ms), "mean_ms_per_day": statistics.mean(ms), "sd": sd,
                                "median_ms_per_day": statistics.median(ms), "mean_fps": statistics.mean(fps)}
        print(f"{n:6s} n={len(ms):2d}  mean {statistics.mean(ms):7.1f} +/- {sd:5.1f}  median {statistics.median(ms):7.1f}"
              f" ms/day  {statistics.mean(fps):5.1f} fps")
    if len(names) == 2:
        a, b = names
        out["paired"] = {}
        for key, label in (("ms_per_day", "ms/day"), ("fps", "fps")):
            diffs = []
            for blk in blocks:  # each ABBA block: mean(B) vs mean(A)
                ma = statistics.mean(r[key] for r in blk if r["config"] == a)
                mb = statistics.mean(r[key] for r in blk if r["config"] == b)
                diffs.append((mb - ma) / ma * 100)
            mean = statistics.mean(diffs)
            half = t95(len(diffs) - 1) * statistics.stdev(diffs) / math.sqrt(len(diffs)) if len(diffs) > 1 else float("nan")
            lower = sum(d < 0 for d in diffs)
            out["paired"][key] = {"b_vs_a_percent": diffs, "mean": mean, "ci95_half": half,
                                  "median": statistics.median(diffs), "blocks_lower": lower}
            verdict = "significant" if not math.isnan(half) and abs(mean) > half else "not significant"
            print(f"\n--- paired {label} ({b} vs {a}, per ABBA block): {', '.join(f'{d:+.1f}%' for d in diffs)}")
            print(f"mean {mean:+.1f}% +/- {half:.1f}% (95% CI, {verdict}); median {statistics.median(diffs):+.1f}%; "
                  f"{b} lower in {lower}/{len(diffs)} blocks")
            # Blocks alternate ABBA / BAAB. A non-linear drift inside a block (a battle fading after a
            # reload) biases the two orders in opposite directions; with order as a factor the effect is
            # the mean of the two order means, and the variance comes from within each order.
            even, odd = diffs[0::2], diffs[1::2]
            if len(even) >= 2 and len(odd) >= 2:
                eff = (statistics.mean(even) + statistics.mean(odd)) / 2
                se = 0.5 * math.sqrt(statistics.variance(even) / len(even) + statistics.variance(odd) / len(odd))
                h = t95(len(even) + len(odd) - 2) * se
                v = "significant" if abs(eff) > h else "not significant"
                out["paired"][key].update({"order_adjusted": eff, "order_adjusted_ci95_half": h,
                                           "abba_mean": statistics.mean(even), "baab_mean": statistics.mean(odd)})
                print(f"order-adjusted: {eff:+.1f}% +/- {h:.1f}% (95% CI, {v}); ABBA blocks "
                      f"{statistics.mean(even):+.1f}%, BAAB blocks {statistics.mean(odd):+.1f}%")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--blocks", type=int, default=4, help="ABBA blocks (4 segments each)")
    ap.add_argument("--days", type=int, default=60, help="timed days per segment")
    ap.add_argument("--settle", type=int, default=3, help="days to let pass after switching settings")
    ap.add_argument("--smooth", type=int, default=1, help="frame smoothing during the run (1 = the game's default)")
    ap.add_argument("--no-fm-check", action="store_true", help="do not require the fleet manager window")
    ap.add_argument("--config", action="append", help="NAME:key=val,... (stellaris_perf.ini keys); give two")
    ap.add_argument("--reload", metavar="SAVE", help="restart the game into this save before every ABBA block "
                    "(game_session.py), so every block starts from the same moment of the game")
    ap.add_argument("--warmup", type=int, default=10, help="days to let pass after each --reload before timing "
                    "(the first minute after a load runs slow)")
    args = ap.parse_args()
    configs = [parse_config(c) for c in args.config] if args.config else [
        ("base", {"fleet_manager_cache": 0, "fleet_manager_reinforce_ms": 0}),
        ("fix", {"fleet_manager_cache": 1, "fleet_manager_reinforce_ms": 1000})]
    if len(configs) != 2:
        raise SystemExit("give exactly two --config")
    if args.reload:
        import game_session
        game_session.load(args.reload, "11_638438808", 400, not args.no_fm_check)
    r = Runner(args)
    if not args.no_fm_check and r.fm_visible(1.0) < 0.9:
        r.wait_for_window()
    records, blocks = [], []
    (a, ca), (b, cb) = configs
    print(f"{args.blocks} ABBA blocks x {args.days} days (+{args.settle} settle), frame_smoothing={args.smooth}")
    try:
        for i in range(args.blocks):
            if args.reload and i > 0:
                r.bench.close()
                game_session.load(args.reload, "11_638438808", 400, not args.no_fm_check)
                r = Runner(args)
            if args.reload:
                r.apply(ca)
                r.wait_days(args.warmup)
            print(f"block {i + 1}/{args.blocks}", flush=True)
            # ABBA, then BAAB: a transient at the start of a block (right after a --reload) falls on
            # each configuration equally often
            order = ((a, ca), (b, cb), (b, cb), (a, ca)) if i % 2 == 0 else ((b, cb), (a, ca), (a, ca), (b, cb))
            blk = [r.segment(n, c) for n, c in order]
            records += blk
            blocks.append(blk)
    finally:
        r.bench.cmd("pause 1")
        write_perf_ini(frame_smoothing=args.smooth, **ca)
    summary = summarize(configs, records, blocks)
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, time.strftime("%Y%m%d_%H%M%S") + "_bench_fm.json")
    with open(path, "w", encoding="utf8") as f:
        json.dump({"args": vars(args), "configs": configs, "records": records, "summary": summary}, f, indent=1,
                  ensure_ascii=False)
    print(f"\nsaved {path}")


if __name__ == "__main__":
    sys.exit(main())
