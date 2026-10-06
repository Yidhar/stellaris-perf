"""Reads stellaris_perf_rules.csv (rule_profile=1) and names each game rule from live memory.

    python rule_report.py [top=25]

Rule names: CScriptedRule::Evaluate builds "CGameRules::<name>.Evaluate" from the rule's index
(int at the start of the rule): index -> int table at RVA RULE_INDEX_TABLE -> row of the string
table object RULE_NAME_TABLE (+0x70 rows, 0x30 bytes each, std::string at +0x10).
The two RVAs are read from the exe's CScriptedRule::Evaluate (sdk::fn::CScriptedRule_Evaluate: the `lea rcx, [rip+..]`
before `movsxd rdi, [rcx+rax*4]` is the index table; the first call after it is an accessor whose `lea rax, [rip+..]` is
the string table object). They are listed per exe build below, and the tool refuses a build that is not listed: this is a
throwaway analysis tool, so re-derive the two RVAs after a game patch and add the build.
"""
import csv
import ctypes
import os
import struct
import sys
from collections import defaultdict

from benchlib import PERF_LOGS, game_pid

# PE TimeDateStamp of stellaris.exe -> (RULE_INDEX_TABLE, RULE_NAME_TABLE)
TABLES = {
    0x6AB5181D: (0x2831870, 0x35F0AC0),  # 4.5.1
    0x6ABEAA3F: (0x2832870, 0x35F22F0),  # 4.5.2
}
TOP = int(sys.argv[1]) if len(sys.argv) > 1 else 25

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
PID = game_pid()
H = k32.OpenProcess(0x10 | 0x400, False, PID)


def rd(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(H, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        raise OSError(ctypes.get_last_error())
    return buf.raw


def image_base():
    import ctypes.wintypes as w
    psapi = ctypes.WinDLL("psapi")
    mods = (ctypes.c_void_p * 1024)()
    need = w.DWORD()
    psapi.EnumProcessModulesEx(ctypes.c_void_p(H), mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
    return mods[0]


BASE = image_base()
_e_lfanew = struct.unpack("<I", rd(BASE + 0x3C, 4))[0]
EXE_TIMESTAMP = struct.unpack("<I", rd(BASE + _e_lfanew + 8, 4))[0]
if EXE_TIMESTAMP not in TABLES:
    raise SystemExit(f"rule_report.py has no table addresses for the exe build 0x{EXE_TIMESTAMP:08X}; derive them (see the "
                     "docstring) and add the build to TABLES")
RULE_INDEX_TABLE, RULE_NAME_TABLE = TABLES[EXE_TIMESTAMP]
rows = struct.unpack("<Q", rd(BASE + RULE_NAME_TABLE + 0x70, 8))[0]


def rule_name(index):
    try:
        j = struct.unpack("<i", rd(BASE + RULE_INDEX_TABLE + index * 4, 4))[0]
        e = rows + j * 0x30
        size, cap = struct.unpack("<QQ", rd(e + 0x20, 16))
        data = rd(e + 0x10, 16) if cap < 16 else rd(struct.unpack("<Q", rd(e + 0x10, 8))[0], size)
        return data[:size].decode("utf-8", "replace")
    except OSError:
        return f"#{index}"


stats = defaultdict(lambda: defaultdict(lambda: [0, 0]))
with open(os.path.join(PERF_LOGS, "stellaris_perf_rules.csv"), encoding="utf-8") as f:
    for r in csv.DictReader(f):
        s = stats[int(r["rule_index"])][r["path"]]
        s[0] += int(r["calls"])
        s[1] += int(r["cycles"])

GHZ = 4.5  # nominal; the csv's ns_per_call column uses the calibrated rate
total_orig = sum(v["orig"][1] for v in stats.values()) or 1
print(f"{'rule':40s} {'orig calls':>11s} {'ns/orig':>8s} {'share':>6s} | {'hit calls':>10s} {'ns/hit':>7s} {'miss':>9s} {'ns/miss':>8s}")
order = sorted(stats, key=lambda i: -stats[i]["orig"][1])
for i in order[:TOP]:
    s = stats[i]

    def ns(p):
        return s[p][1] / s[p][0] / GHZ if s[p][0] else float("nan")

    print(f"{rule_name(i)[:40]:40s} {s['orig'][0]:11d} {ns('orig'):8.0f} {s['orig'][1] / total_orig:6.1%} | "
          f"{s['hit'][0]:10d} {ns('hit'):7.0f} {s['miss'][0]:9d} {ns('miss'):8.0f}")
print(f"\n{len(stats)} rules; ns assume {GHZ} GHz TSC (see the csv's ns_per_call for the calibrated value)")
