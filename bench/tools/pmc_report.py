"""Attributes hardware-counter samples (cache_misses.wprp) to Stellaris functions and DLLs.

    python pmc_report.py <trace.etl> [top=30]

Runs `xperf -a dumper` (Windows Performance Toolkit) to text, reads the column layout from the dump's
own header lines, keeps the stellaris.exe samples, and groups them by counter (ProfileSource) and by
function: stellaris.exe addresses map to their .pdata function (read from the exe in the Steam library, or from
STELLARIS_DIR), other modules count per module. Prints per function: samples per counter and the miss density
(cache-miss samples per cycle sample, scaled by the sampling intervals).
"""
import bisect
import collections
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from stellaris_paths import require_game_dir  # noqa: E402


def find_xperf():
    """XPERF if set, else xperf.exe on PATH, else the Windows Performance Toolkit of the Windows Kit."""
    env = os.environ.get("XPERF")
    if env:
        return env
    found = shutil.which("xperf")
    if found:
        return found
    for var in ("ProgramFiles(x86)", "ProgramFiles"):
        base = os.environ.get(var)
        if base:
            candidate = os.path.join(base, "Windows Kits", "10", "Windows Performance Toolkit", "xperf.exe")
            if os.path.isfile(candidate):
                return candidate
    raise SystemExit("xperf.exe not found: install the Windows Performance Toolkit, or set XPERF to its path")


class Pdata:
    """The function starts of a PE32+ image, from its exception directory (.pdata): ip -> start of its function."""

    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit(f"{path} is not a PE file")
        nsec, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x20B:
            raise SystemExit(f"{path} is not a 64-bit image")
        exc_rva, exc_size = struct.unpack_from("<II", data, opt + 112 + 3 * 8)
        sections = []
        for i in range(nsec):
            o = opt + opt_size + i * 40
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            sections.append((vaddr, max(vsize, rawsize), rawptr))
        off = next(rawptr + (exc_rva - vaddr) for vaddr, size, rawptr in sections if vaddr <= exc_rva < vaddr + size)
        self.begins = sorted(struct.unpack_from("<I", data, off + i)[0] for i in range(0, exc_size - 11, 12))

    def fn_of(self, rva):
        i = bisect.bisect_right(self.begins, rva) - 1
        return self.begins[i] if i >= 0 else None


INTERVAL = {"DcacheMisses": 65536, "CacheMisses": 65536, "TotalCycles": 1048576}

etl = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
dump = etl + ".txt"
if not os.path.exists(dump):
    subprocess.run([find_xperf(), "-i", etl, "-o", dump, "-a", "dumper"], check=True)

im = Pdata(os.path.join(require_game_dir(), "stellaris.exe"))

headers, images, samples = {}, [], []
for line in open(dump, encoding="utf-8", errors="replace"):
    cols = [c.strip() for c in line.split(",")]
    if not cols or not cols[0]:
        continue
    ev = cols[0]
    if ev.startswith("BeginHeader") or ev.startswith("EndHeader"):
        continue
    if ev not in headers and len(cols) > 3 and cols[1] == "TimeStamp":
        headers[ev] = {name: i for i, name in enumerate(cols)}  # header line of this event type
        continue
    h = headers.get(ev)
    if not h:
        continue
    if ev.startswith("I-DCStart") or ev.startswith("I-Start") or ev.startswith("Image"):
        try:
            base = int(cols[h.get("ImageBase", 3)], 16)
            end = int(cols[h.get("ImageEnd", 4)], 16)
            name = os.path.basename(cols[h.get("FileName", len(cols) - 1)].strip('"'))
            proc = cols[h.get("Process Name ( PID)", 2)]
            images.append((base, end, name, proc))
        except (ValueError, IndexError):
            pass
    elif ev.startswith("PmcInterrupt") or ev.startswith("SampledProfile"):
        proc = cols[h.get("Process Name ( PID)", 2)]
        if "stellaris.exe" not in proc.lower():
            continue
        ip_col = next((h[k] for k in ("InstructionPointer", "PrgrmCtr", "IP") if k in h), None)
        src_col = next((h[k] for k in ("ProfileSource", "Source") if k in h), None)
        if ip_col is None:
            continue
        try:
            ip = int(cols[ip_col], 16)
        except ValueError:
            continue
        src = cols[src_col] if src_col is not None else ("Timer" if ev.startswith("Sampled") else "?")
        samples.append((proc, ip, src))

if not samples:
    raise SystemExit(f"no stellaris.exe PMC samples found; event types in the dump: {sorted(headers)[:40]}")

mods = sorted({(b, e, n) for b, e, n, p in images if "stellaris.exe" in p.lower()})
exe = next(((b, e) for b, e, n in mods if n.lower() == "stellaris.exe"), None)


def where(ip):
    if exe and exe[0] <= ip < exe[1]:
        f = im.fn_of(ip - exe[0])
        return f"stellaris.exe!0x{f:x}" if f is not None else f"stellaris.exe+0x{ip - exe[0]:x}"
    for b, e, n in mods:
        if b <= ip < e:
            return n
    return "kernel/unknown" if ip >= 0xFFFF000000000000 else "unknown"


count = collections.defaultdict(collections.Counter)
for proc, ip, src in samples:
    count[where(ip)][src] += 1
sources = sorted({s for _, _, s in samples})
totals = collections.Counter(s for _, _, s in samples)
print("samples per counter:", dict(totals))
key = "CacheMisses" if "CacheMisses" in sources else sources[0]
print(f"\n{'function / module':44s} " + " ".join(f"{s:>13s}" for s in sources) + "   miss/kcycle")
for w_, c in sorted(count.items(), key=lambda kv: -kv[1][key])[:top]:
    dens = ""
    if c.get("TotalCycles") and c.get(key):
        dens = f"{c[key] * INTERVAL.get(key, 1) / (c['TotalCycles'] * INTERVAL['TotalCycles']) * 1000:10.2f}"
    print(f"{w_[:44]:44s} " + " ".join(f"{c.get(s, 0):13d}" for s in sources) + f"   {dens}")
