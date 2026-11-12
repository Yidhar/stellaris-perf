"""Live check of the GUI caches (ui_power_cache, tooltip_cache, fleet_upgrade_cache, cursor_path_cache) without the
real mouse or keyboard: the bench DLL selects a fleet and answers the engine's mouse position query.

    python gui_check.py load [--save fmbase] [--folder 11_638438808]   start the game on the save, mods off, and select
                                                                       the player's largest fleet (the fleet window opens)
    python gui_check.py scan                 a grid of mouse positions: where tooltips cost something, where the cursor's
                                             path check fires (galaxy view, mouse on a system)
    python gui_check.py verify X,Y [X,Y...]  the caches in verify mode at those positions: mismatches, the engine's cost
                                             per call
    python gui_check.py ab X,Y [--run]       every cache off / on, twice, at that position (--run: unpaused at speed 3)
    python gui_check.py close                close the game `load` started and put dlc_load.json / continue_game.json back

`load` refuses to run while any stellaris.exe exists (it never closes a game it did not start). The save needs a
player with fleets; the galaxy view (posted `M` key) is needed for the path check. Results are printed; nothing is
written to the repository.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

import ab_setting
import benchlib
import dllctl
import game_session
from benchlib import game_pids, stellaris_data_dir

user32 = ctypes.WinDLL("user32", use_last_error=True)
DOCS = stellaris_data_dir()
STATE = os.path.join(tempfile.gettempdir(), "stellaris_perf_gui_check")
FILES = ("dlc_load.json", "continue_game.json")
LOG = os.path.join(benchlib.PERF_LOGS, "stellaris_perf.log")
COUNTERS = ("fleet_view_updates", "fleet_view_ns", "ui_power_hits", "ui_power_misses", "tooltip_kept", "tooltip_built", "tooltip_bypassed",
            "tooltip_mismatches", "tooltip_build_ns", "upgrade_kept", "upgrade_asked", "upgrade_mismatches", "upgrade_ns", "path_kept",
            "path_asked", "path_mismatches", "path_ns")
OFF = dict(ui_power_cache=0, tooltip_cache=0, fleet_upgrade_cache=0, cursor_path_cache=0)
ON = dict(ui_power_cache=1, tooltip_cache=1, fleet_upgrade_cache=1, cursor_path_cache=1)
VERIFY = dict(ui_power_cache=1, tooltip_cache=2, fleet_upgrade_cache=2, cursor_path_cache=2)


# ---- the player's fleets, from the save ---------------------------------------------------------------------------

def block(text, start):
    depth, i = 0, start
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
        elif c == '"':
            i = text.index('"', i + 1)
        i += 1
    raise ValueError("unterminated block")


def top(text, key):
    m = re.search(r"(?m)^" + re.escape(key) + r"=\s*\{", text)
    if not m:
        raise SystemExit(f"the save has no top-level {key}")
    return block(text, m.end() - 1)


def player_fleets(path):
    """[(fleet id, ships)] of the player's mobile fleets, largest first."""
    with zipfile.ZipFile(path) as z:
        text = z.read("gamestate").decode("utf-8", errors="replace")
    country = int(re.search(r"country=(\d+)", top(text, "player")).group(1))
    countries = top(text, "country")
    me = block(countries, re.search(r"(?m)^\t" + str(country) + r"=\s*\{", countries).end() - 1)
    owned = re.search(r"owned_fleets=\s*\{", me)
    ids = [int(x) for x in re.findall(r"fleet=(\d+)", block(me, owned.end() - 1))] if owned else []
    fleets = top(text, "fleet")
    out = []
    for fid in ids:
        m = re.search(r"(?m)^\t" + str(fid) + r"=\s*\{", fleets)
        if not m:
            continue
        body = block(fleets, m.end() - 1)
        ships = re.search(r"\bships=\s*\{([^}]*)\}", body)
        if ships and not re.search(r"\bstation=yes", body):
            out.append((fid, len(ships.group(1).split())))
    return sorted(out, key=lambda r: -r[1])


# ---- helpers ------------------------------------------------------------------------------------------------------

def settings(**over):
    benchlib.write_perf_ini(**dict(ab_setting.DEFAULTS, **over))
    time.sleep(4.5)  # the plugin re-reads its ini every 2 s and publishes its counters every 2 s


def delta(perf, bench, seconds):
    time.sleep(2.2)
    a, sa = perf.read(), bench.status()
    time.sleep(seconds + 2.2)
    b, sb = perf.read(), bench.status()
    d = {k: b[k] - a[k] for k in COUNTERS}
    d["seconds"], d["days"] = sb["t"] - sa["t"], sb["day"] - sa["day"]
    return d


def ms(ns, n):
    return "-" if not n else f"{ns / 1e6 / n:.3f}"


def show(label, d):
    asked = d["tooltip_kept"] + d["tooltip_built"] + d["tooltip_bypassed"]
    print(f"[{label}] {d['seconds']:.1f} s, {d['days']} days | fleet entry updates {d['fleet_view_updates']} at "
          f"{ms(d['fleet_view_ns'], d['fleet_view_updates'])} ms | power hits {d['ui_power_hits']} misses {d['ui_power_misses']}"
          f" | tooltip kept {d['tooltip_kept']} built {d['tooltip_built']} mismatches {d['tooltip_mismatches']} "
          f"({ms(d['tooltip_build_ns'], asked)} ms per provider call) | upgrade kept {d['upgrade_kept']} asked {d['upgrade_asked']} mismatches "
          f"{d['upgrade_mismatches']} ({ms(d['upgrade_ns'], d['upgrade_asked'])} ms each) | path kept {d['path_kept']} asked {d['path_asked']} "
          f"mismatches {d['path_mismatches']} ({ms(d['path_ns'], d['path_asked'])} ms each)", flush=True)


def post_key(pid, vk):
    """A key press through the window's message queue: the game takes it without being in the foreground."""
    hwnd = game_session.game_window(pid)
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.PostMessageW(hwnd, 0x100, vk, 1 | (scan << 16))
    time.sleep(0.08)
    user32.PostMessageW(hwnd, 0x101, vk, 1 | (scan << 16) | (1 << 30) | (1 << 31))


def point(text):
    x, y = text.split(",")
    return int(x), int(y)


# ---- commands -----------------------------------------------------------------------------------------------------

def load(a):
    if game_pids():
        raise SystemExit("a stellaris.exe is running: not started by this script, so it is left alone. Nothing was changed.")
    os.makedirs(STATE, exist_ok=True)
    for name in FILES:
        src = os.path.join(DOCS, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(STATE, name))
    with open(os.path.join(DOCS, "dlc_load.json"), "w", encoding="utf-8") as f:
        f.write('{"disabled_dlcs":[],"enabled_mods":[]}')
    fleets = player_fleets(os.path.join(DOCS, "save games", a.folder, a.save + ".sav"))
    if not fleets:
        raise SystemExit("the player has no mobile fleet in this save")
    benchlib.write_perf_ini(**dict(ab_setting.DEFAULTS, **VERIFY))
    game_session.load(a.save, a.folder, 400)
    pid = game_pids()[0]
    with open(os.path.join(STATE, "pid"), "w") as f:
        f.write(str(pid))
    bench = benchlib.Bench(tries=100)
    r = bench.cmd(f"select {fleets[0][0]}")
    bench.close()
    print(f"selected the player's largest fleet {fleets[0]}: {r}")
    time.sleep(5)
    st = benchlib.PerfStats(pid).read()
    print({k: st[k] for k in ("ui_power_cache", "tooltip_cache", "fleet_upgrade_cache", "cursor_path_cache", "fleet_view_hooked")})
    if a.galaxy:
        post_key(pid, 0x4D)  # M: the galaxy map
        print("posted M (galaxy map)")


def close(_):
    try:
        pid = int(open(os.path.join(STATE, "pid")).read())
    except (OSError, ValueError):
        pid = None
    if pid in game_pids():
        for key in ("perf", "bench"):
            try:
                dllctl.unload(pid, key)
            except Exception as e:  # the DLL may not be loaded
                print(f"unload {key}: {e}")
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    for name in FILES:
        saved = os.path.join(STATE, name)
        if os.path.exists(saved):
            shutil.copyfile(saved, os.path.join(DOCS, name))
            os.remove(saved)
    if os.path.exists(benchlib.PERF_INI):
        os.remove(benchlib.PERF_INI)
    print("closed the test game, restored dlc_load.json and continue_game.json, removed the test ini")


def scan(a, bench, perf):
    settings(**VERIFY)
    for y in range(60, 700, 80):
        for x in range(40, 1280, 120):
            bench.cmd(f"mouse {x} {y}")
            time.sleep(0.25)
            p0, st = perf.read(), bench.status()
            time.sleep(2.3)
            p1 = perf.read()
            calls = p1["tooltip_kept"] + p1["tooltip_built"] - p0["tooltip_kept"] - p0["tooltip_built"]
            print({"x": x, "y": y, "hovered": st["hovered"], "tooltip_ms": ms(p1["tooltip_build_ns"] - p0["tooltip_build_ns"], calls),
                   "path_checks": p1["path_asked"] - p0["path_asked"]}, flush=True)


def verify(a, bench, perf):
    settings(**VERIFY)
    for x, y in a.points:
        bench.cmd(f"mouse {x} {y}")
        time.sleep(0.5)
        show(f"verify, mouse at {x},{y}", delta(perf, bench, a.seconds))
    lines = [l.strip() for l in open(LOG, encoding="utf-8", errors="replace") if " verify" in l]
    print(f"{len(lines)} verify lines in the plugin log" + (":" if lines else ""))
    for l in lines[-6:]:
        print("  " + l[:240])


def ab(a, bench, perf):
    x, y = a.points[0]
    bench.cmd(f"mouse {x} {y}")
    if a.run:
        bench.cmd("speed 3")
        bench.cmd("pause 0")
    for label, over in (("off", OFF), ("on", ON), ("off", OFF), ("on", ON)):
        settings(**over)
        show(f"caches {label}, mouse at {x},{y}", delta(perf, bench, a.seconds))
    if a.run:
        bench.cmd("pause 1")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["load", "scan", "verify", "ab", "close"])
    ap.add_argument("points", nargs="*", type=point, help="mouse positions X,Y in the game window's client area")
    ap.add_argument("--save", default="fmbase")
    ap.add_argument("--folder", default="11_638438808")
    ap.add_argument("--galaxy", action="store_true", help="load: switch to the galaxy map afterwards")
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--run", action="store_true", help="ab: unpaused at speed 3")
    a = ap.parse_args()
    if a.action == "load":
        return load(a)
    if a.action == "close":
        return close(a)
    if a.action in ("verify", "ab") and not a.points:
        raise SystemExit("give at least one mouse position X,Y")
    pid = benchlib.game_pid()
    bench, perf = benchlib.Bench(tries=50), benchlib.PerfStats(pid)
    try:
        {"scan": scan, "verify": verify, "ab": ab}[a.action](a, bench, perf)
    finally:
        bench.cmd("mouse off")
        bench.close()


if __name__ == "__main__":
    sys.exit(main())
