"""Shared helpers for the stellaris_bench scripts: the game process, the bench pipe, the perf DLL's
statistics and its ini."""
import ctypes
import ctypes.wintypes as w
import json
import mmap
import os
import struct
import time

from stellaris_paths import game_dir, require_game_dir, stellaris_data_dir  # noqa: F401

GAME_DIR = game_dir()  # None when the game is not found; scripts that need it call require_game_dir()
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def perf_dir():
    """The folder stellaris_perf.dll is loaded from, which is also where it reads config\\stellaris_perf.ini and writes logs\\:
    STELLARIS_PERF_DIR, else the folder assembled by the build (build\\plugin\\stellaris-perf), else the installed plugin
    (<Documents>\\Paradox Interactive\\Stellaris\\plugins\\stellaris-perf)."""
    env = os.environ.get("STELLARIS_PERF_DIR")
    if env:
        return env
    installed = os.path.join(stellaris_data_dir(), "plugins", "stellaris-perf")
    built = os.path.join(REPO, "build", "plugin", "stellaris-perf")
    for p in (built, installed):
        if os.path.exists(os.path.join(p, "stellaris_perf.dll")):
            return p
    return installed


PERF_DIR = perf_dir()
PERF_INI = os.path.join(PERF_DIR, "config", "stellaris_perf.ini")
PERF_LOGS = os.path.join(PERF_DIR, "logs")
BENCH_PIPE = r"\\.\pipe\stellaris_bench"

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x2, 0x8, 0x10


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
                ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", w.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("th32ModuleID", w.DWORD), ("th32ProcessID", w.DWORD),
                ("GlblcntUsage", w.DWORD), ("ProccntUsage", w.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", w.DWORD), ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


kernel32.CreateToolhelp32Snapshot.restype = w.HANDLE
kernel32.Process32FirstW.argtypes = kernel32.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Module32FirstW.argtypes = kernel32.Module32NextW.argtypes = [w.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
kernel32.CloseHandle.argtypes = [w.HANDLE]


def game_pids():
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
    pids = []
    ok = kernel32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == "stellaris.exe":
            pids.append(e.th32ProcessID)
        ok = kernel32.Process32NextW(snap, ctypes.byref(e))
    kernel32.CloseHandle(snap)
    return pids


def game_pid():
    pids = game_pids()
    if len(pids) != 1:
        raise SystemExit(f"expected exactly one stellaris.exe, found {pids}")
    return pids[0]


def module_loaded(pid, name):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap in (None, -1, w.HANDLE(-1).value):
        return False
    e = MODULEENTRY32W(dwSize=ctypes.sizeof(MODULEENTRY32W))
    found = False
    ok = kernel32.Module32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szModule.lower() == name.lower():
            found = True
            break
        ok = kernel32.Module32NextW(snap, ctypes.byref(e))
    kernel32.CloseHandle(snap)
    return found


class Bench:
    """One persistent connection to stellaris_bench.dll; every command runs on the game's main thread."""

    def __init__(self, tries=50):
        err = None
        for _ in range(tries):
            try:
                self.f = open(BENCH_PIPE, "r+b", buffering=0)
                return
            except OSError as e:
                err = e
                time.sleep(0.1)
        raise SystemExit(f"cannot open {BENCH_PIPE}: {err} (is stellaris_bench.dll loaded?)")

    def cmd(self, line):
        self.f.write((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.f.read(1)
            if not chunk:
                raise SystemExit("bench pipe closed")
            buf += chunk
        return json.loads(buf)

    def status(self):
        return self.cmd("status")

    def close(self):
        self.f.close()


# perf_shared.hpp: SharedStats
PERF_STATS_FMT = "<IIQiiiiiiiiQQQQQQQQQQQ"
PERF_STATS_FIELDS = ("magic", "size", "applied_seq", "frame_smoothing", "opinion_cache", "rule_cache",
                     "fleet_manager_cache", "fleet_manager_reinforce_ms", "flag_simd", "flag_expiry_skip", "fleet_parallel_grain1",
                     "fleet_manager_updates", "reinforce_skipped", "reinforce_passed", "fleet_power_hits",
                     "fleet_power_misses", "flag_static", "flag_dynamic", "flag_unreadable", "flag_mismatch",
                     "expiry_skipped", "expiry_passed")
PERF_MAGIC = 0x31465250


class PerfStats:
    """Read-only view of stellaris_perf.dll's statistics (Local\\stellaris_perf_stats_<pid>)."""

    def __init__(self, pid):
        size = struct.calcsize(PERF_STATS_FMT)
        # opening by name maps the DLL's section; a fresh (zeroed) one means the DLL is not loaded
        self.m = mmap.mmap(-1, size, tagname=f"Local\\stellaris_perf_stats_{pid}", access=mmap.ACCESS_READ)
        if self.read()["magic"] != PERF_MAGIC:
            raise SystemExit("stellaris_perf.dll statistics not found (is the current stellaris_perf.dll loaded?)")

    def read(self):
        self.m.seek(0)
        return dict(zip(PERF_STATS_FIELDS, struct.unpack(PERF_STATS_FMT, self.m.read(struct.calcsize(PERF_STATS_FMT)))))


def write_perf_ini(frame_smoothing=-1, opinion_cache=0, rule_cache=0, fleet_manager_cache=0,
                   fleet_manager_reinforce_ms=0, flag_simd=0, flag_expiry_skip=0, profile=0, rule_profile=0,
                   modifier_flush=0, modifier_flush_max=32, fleet_parallel_grain1=0,
                   scope_profile=0):
    os.makedirs(os.path.dirname(PERF_INI), exist_ok=True)
    with open(PERF_INI, "w") as f:
        f.write("[perf]\n"
                f"frame_smoothing={frame_smoothing}\n"
                f"opinion_cache={opinion_cache}\n"
                f"rule_cache={rule_cache}\n"
                f"fleet_manager_cache={fleet_manager_cache}\n"
                f"fleet_manager_reinforce_ms={fleet_manager_reinforce_ms}\n"
                f"flag_simd={flag_simd}\n"
                f"flag_expiry_skip={flag_expiry_skip}\n"
                f"profile={profile}\n"
                f"rule_profile={rule_profile}\n"
                f"modifier_flush={modifier_flush}\n"
                f"modifier_flush_max={modifier_flush_max}\n"
                f"fleet_parallel_grain1={fleet_parallel_grain1}\n"
                f"scope_profile={scope_profile}\n")
