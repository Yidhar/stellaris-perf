"""Restarts Stellaris straight into a save and prepares it for a benchmark, without the menus.

    python game_session.py load <save name> [--folder "11_638438808"] [--timeout 400]
    python game_session.py restore        # put the original continue_game.json back

`load`: lets stellaris_perf/bench unload themselves, closes the game, points continue_game.json at the
save, starts `stellaris.exe -dx11 --continuelastsave` (the engine's own "continue" path) from the game
directory, loads both DLLs, waits until the save is in game, then pauses at speed 4.
The first `load` keeps a copy of the original continue_game.json (continue_game.json.perfbench_backup).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

import ctypes
import ctypes.wintypes as w

import dllctl
from benchlib import GAME_DIR, Bench, PerfStats, game_pids

user32 = ctypes.WinDLL("user32", use_last_error=True)
VK = {"F9": 0x78, "z": 0x5A}
KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE, INPUT_KEYBOARD = 0x2, 0x8, 1


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", w.WORD), ("wScan", w.WORD), ("dwFlags", w.DWORD), ("time", w.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", w.DWORD), ("u", _U)]


def game_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(hwnd, _):
        owner = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and not user32.GetWindow(hwnd, 4):  # GW_OWNER
            found.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def press(pid, key):
    """Brings the game window to the front and sends one key press (scan codes, as the game reads raw input)."""
    hwnd = game_window(pid)
    if not hwnd:
        return False
    user32.keybd_event(0x12, 0, 0, 0)  # an Alt tap lets this process move the foreground
    user32.keybd_event(0x12, 0, KEYEVENTF_KEYUP, 0)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    scan = user32.MapVirtualKeyW(VK[key], 0)
    for flags in (KEYEVENTF_SCANCODE, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP):
        inp = INPUT(type=INPUT_KEYBOARD, ki=KEYBDINPUT(wVk=0, wScan=scan, dwFlags=flags, time=0, dwExtraInfo=None))
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.05)
    return True


def open_fleet_manager(pid, tries=4):
    """F9 opens the military view (fleet manager / designer tabs); `z` selects the fleet manager tab.
    Open means CFleetManagerView::Update runs (stellaris_perf.dll's statistics)."""
    perf = PerfStats(pid)

    def updating():
        a = perf.read()["fleet_manager_updates"]
        time.sleep(1.0)
        return perf.read()["fleet_manager_updates"] - a > 5

    if updating():
        return True
    for i in range(tries):
        press(pid, "F9" if i % 2 == 0 else "z")
        time.sleep(1.0)
        if updating():
            return True
    return False

DOCS = os.path.join(os.path.expanduser("~"), "Documents", "Paradox Interactive", "Stellaris")
CONTINUE = os.path.join(DOCS, "continue_game.json")
BACKUP = CONTINUE + ".perfbench_backup"


def close_game(timeout=60):
    pids = game_pids()
    if not pids:
        return
    if len(pids) == 1:
        for key in ("perf", "bench"):
            try:
                dllctl.unload(pids[0], key)
            except Exception as e:  # the game may already be gone
                print(f"unload {key}: {e}")
    subprocess.run(["taskkill", "/F", "/IM", "stellaris.exe"], capture_output=True)
    deadline = time.time() + timeout
    while game_pids() and time.time() < deadline:
        time.sleep(0.5)
    if game_pids():
        raise SystemExit("stellaris.exe did not exit")


def point_continue_at(folder, name):
    if not os.path.exists(BACKUP) and os.path.exists(CONTINUE):
        shutil.copyfile(CONTINUE, BACKUP)
    path = os.path.join(DOCS, "save games", folder, name + ".sav")
    if not os.path.exists(path):
        raise SystemExit(f"no save {path}")
    with open(CONTINUE, "w", encoding="utf-8") as f:
        json.dump({"title": f"save games/{folder}/{name}", "desc": name, "date": ""}, f, ensure_ascii=False, indent="\t")


def load(name, folder, timeout, fleet_manager=False):
    close_game()
    point_continue_at(folder, name)
    subprocess.Popen([os.path.join(GAME_DIR, "stellaris.exe"), "-dx11", "--continuelastsave"], cwd=GAME_DIR,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    deadline = time.time() + timeout
    while len(game_pids()) != 1:
        if time.time() > deadline:
            raise SystemExit("stellaris.exe did not start")
        time.sleep(1)
    pid = game_pids()[0]
    time.sleep(5)  # let the exe map its image and the loader settle before the remote LoadLibrary
    for key in ("bench", "perf"):
        if not dllctl.load(pid, key):
            raise SystemExit(f"could not load {key}")
    bench = Bench(tries=200)
    last, stable = None, 0
    while time.time() < deadline:
        try:
            st = bench.status()
        except SystemExit:
            st = {"ok": False}
        if st.get("ok") and st.get("in_game") and st.get("hours", 0) > 0:
            stable = stable + 1 if st["hours"] == last else 0
            last = st["hours"]
            if stable >= 3:  # in game, date not moving (loaded games start paused)
                break
        time.sleep(1)
    else:
        raise SystemExit("the save did not finish loading in time")
    bench.cmd("pause 1")
    bench.cmd("speed 4")
    if fleet_manager and not open_fleet_manager(pid):
        raise SystemExit("could not open the fleet manager (F9 / z)")
    st = bench.status()
    bench.close()  # the bench pipe serves one client at a time
    print(f"loaded {name}: day {st['day']} paused={st['paused']} speed={st['speed']}")
    return st


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["load", "restore"])
    ap.add_argument("name", nargs="?")
    ap.add_argument("--folder", default="11_638438808")
    ap.add_argument("--timeout", type=float, default=400)
    ap.add_argument("--fleet-manager", action="store_true", help="open the fleet manager after loading (F9)")
    a = ap.parse_args()
    if a.action == "restore":
        if os.path.exists(BACKUP):
            shutil.copyfile(BACKUP, CONTINUE)
            print(f"restored {CONTINUE}")
        return 0
    if not a.name:
        raise SystemExit("load needs a save name")
    load(a.name, a.folder, a.timeout, a.fleet_manager)
    return 0


if __name__ == "__main__":
    sys.exit(main())
