"""Reads stellaris_perf_rules.csv (rule_profile=1): the time spent in each game rule, by cache path.

    python rule_report.py [top=25]

The plugin writes every rule's name next to its index (the token CScriptedRule::Evaluate builds
"CGameRules::<name>.Evaluate" from, read through the SDK's addresses), so the report needs neither the running game
nor addresses of its own. A csv written by a plugin before that has only the index.
"""
import csv
import os
import sys
from collections import defaultdict

from benchlib import PERF_LOGS

TOP = int(sys.argv[1]) if len(sys.argv) > 1 else 25
names = {}


def rule_name(index):
    return names.get(index) or f"#{index}"


stats = defaultdict(lambda: defaultdict(lambda: [0, 0]))
with open(os.path.join(PERF_LOGS, "stellaris_perf_rules.csv"), encoding="utf-8") as f:
    for r in csv.DictReader(f):
        s = stats[int(r["rule_index"])][r["path"]]
        if r.get("rule"):
            names[int(r["rule_index"])] = r["rule"]
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
