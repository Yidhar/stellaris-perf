"""Checks that a stellaris_perf.ini setting leaves a save file's bytes unchanged.

    python save_compare.py <key> [--save late_base] [--folder 11_638438808] [--rounds 2] [--off 0] [--on 1]

Loads the save (game_session.py), pauses, and from that one paused game saves with <key>=<off>, then with
<key>=<on>, <rounds> times over (the bench DLL's `save` command, CInGameIdler::SaveGame into the campaign's folder),
then compares every entry of the archives (gamestate, meta) byte for byte: off against on in each round, and off
against off / on against on across rounds, which shows what changes between two saves of a paused game anyway.
Two runs of the same save cannot be compared this way (the engine's own threading makes them diverge); two saves of
one paused game can. The other keys are ab_setting.DEFAULTS. The test saves (sc_<n>_off / sc_<n>_on) are deleted
afterwards.
"""
import argparse
import os
import sys
import time
import zipfile

import ab_setting
import benchlib
import game_session


def write_setting(settings, key, value):
    settings[key] = value
    benchlib.write_perf_ini(**settings)
    time.sleep(3.5)  # the plugin re-reads the ini every 2 s


def save(bench, folder, name):
    path = os.path.join(folder, name + ".sav")
    if os.path.exists(path):
        os.remove(path)
    r = bench.cmd("save " + name)
    if not r.get("ok"):
        raise SystemExit(f"save {name}: {r}")
    t0, last, stable = time.time(), -1, 0
    while time.time() - t0 < 180:
        time.sleep(0.5)
        if os.path.exists(path):
            size = os.path.getsize(path)
            stable = stable + 1 if size == last and size > 0 else 0
            last = size
            if stable >= 4:
                return path
    raise SystemExit(f"{name}: no finished file after 180 s")


def entries(path):
    with zipfile.ZipFile(path) as z:
        return {n: z.read(n) for n in z.namelist()}


def compare(a, b, data):
    same_all = True
    for e in sorted(set(data[a]) | set(data[b])):
        x, y = data[a].get(e), data[b].get(e)
        same = x == y
        same_all = same_all and same
        print(f"  {a} vs {b}: {e}: {'same' if same else 'DIFFERENT'} ({len(x) if x else 0} / {len(y) if y else 0} bytes)")
        if not same and x and y:
            n = min(len(x), len(y))
            i = next((i for i in range(n) if x[i] != y[i]), n)
            print(f"    first difference at {i}: {x[max(0, i - 80):i + 80]!r}")
            print(f"                        vs {y[max(0, i - 80):i + 80]!r}")
    return same_all


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("key")
    ap.add_argument("--save", default="late_base")
    ap.add_argument("--folder", default="11_638438808")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--off", type=int, default=0)
    ap.add_argument("--on", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=400)
    a = ap.parse_args()
    if a.key not in ab_setting.KEYS:
        raise SystemExit(f"unknown key {a.key}; one of {', '.join(ab_setting.KEYS)}")
    folder = os.path.join(benchlib.stellaris_data_dir(), "save games", a.folder)
    settings = dict(ab_setting.DEFAULTS)
    settings[a.key] = a.off
    benchlib.write_perf_ini(**settings)
    game_session.load(a.save, a.folder, a.timeout)
    bench = benchlib.Bench(tries=100)
    bench.cmd("pause 1")
    time.sleep(2)
    names = []
    try:
        for rnd in range(a.rounds):
            for arm, value in (("off", a.off), ("on", a.on)):
                write_setting(settings, a.key, value)
                name = f"sc_{rnd}_{arm}"
                path = save(bench, folder, name)
                names.append(name)
                print(f"{name}: {a.key}={value}, {os.path.getsize(path)} bytes", flush=True)
        data = {n: entries(os.path.join(folder, n + ".sav")) for n in names}
        ok = True
        print("off against on, each round:")
        for rnd in range(a.rounds):
            ok = compare(f"sc_{rnd}_off", f"sc_{rnd}_on", data) and ok
        if a.rounds > 1:
            print("the same arm across rounds (what a paused game changes between saves anyway):")
            compare("sc_0_off", "sc_1_off", data)
            compare("sc_0_on", "sc_1_on", data)
        print(f"{a.key}: {'same bytes' if ok else 'DIFFERENT bytes'} with {a.off} and {a.on}")
    finally:
        bench.close()
        for n in names:
            try:
                os.remove(os.path.join(folder, n + ".sav"))
            except OSError:
                pass
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
