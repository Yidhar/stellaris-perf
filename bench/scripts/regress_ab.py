"""regress_ab.py <prefix>: arm effect with group and position fixed effects (log scale) over <prefix>_*.json from ab_setting.py (needs numpy)."""
import glob
import json
import math
import sys

import numpy as np

rows = []
for gi, f in enumerate(sorted(glob.glob(sys.argv[1] + "_*.json"))):
    segs = json.load(open(f))["segments"]
    if len(segs) < 4:
        continue
    for pos, s in enumerate(segs):
        rows.append((gi, pos, 1 if s["arm"] == "B" else 0, s["ms_day"], s["cpu_ms_day"]))
G = sorted({r[0] for r in rows})
T = {10: 2.23, 14: 2.14, 15: 2.13, 20: 2.09, 25: 2.06, 30: 2.04}
for col, label in ((3, "wall"), (4, "CPU")):
    X = np.array([[1.0 if r[0] == g else 0.0 for g in G] + [1.0 if r[1] == p else 0.0 for p in (1, 2, 3)] + [float(r[2])] for r in rows])
    y = np.log(np.array([r[col] for r in rows]))
    beta, _, rank, _ = np.linalg.lstsq(X, y, rcond=None)
    resid = y - X @ beta
    df = len(rows) - rank
    cov = (resid @ resid / df) * np.linalg.inv(X.T @ X)
    b, se = beta[-1], math.sqrt(cov[-1, -1])
    tc = T[min(T, key=lambda k: abs(k - df))]
    print(f"{label}: B vs A {(math.exp(b) - 1) * 100:+.1f}%  95% CI [{(math.exp(b - tc * se) - 1) * 100:+.1f}, {(math.exp(b + tc * se) - 1) * 100:+.1f}]  (df {df}); position effects " +
          " ".join(f"{(math.exp(p) - 1) * 100:+.0f}%" for p in beta[len(G):len(G) + 3]))
