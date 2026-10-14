"""The surface reads of a flash-watch run as runs of one hash: which states lie between two texts, and how long each lasts.

    python surface-runs.py <work dir> [first N changes]
"""
import csv
import sys
from collections import Counter
from pathlib import Path

d = Path(sys.argv[1])
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 12
freq = 1e7
rows = list(csv.DictReader(open(d / "c-surface.csv")))
runs = []
for r in rows:
    h = r["hash"]
    if runs and runs[-1][0] == h:
        runs[-1][2] = int(r["qpc1"]); runs[-1][3] += 1
    else:
        runs.append([h, int(r["qpc0"]), int(r["qpc1"]), 1])
count = Counter(h for h, *_ in runs)
short = [r for r in runs if r[3] <= 3]
print(f"{len(rows)} reads, {len(runs)} runs, {len(count)} hashes; runs of 1-3 reads: {len(short)}, hashes {Counter(r[0] for r in short).most_common(4)}")
t0 = runs[0][1]
for i, (h, a, b, n) in enumerate(runs[: limit * 2 + 1]):
    print(f"{(a - t0) / freq:9.4f} s  {h}  reads {n:5d}  {(b - a) / freq * 1000:9.2f} ms  seen {count[h]}x")
# The gap before and after each short state: was a read of the old or new full picture within 1 ms?
gaps = []
for i in range(1, len(runs) - 1):
    if runs[i][3] <= 3 and count[runs[i][0]] > 3:
        gaps.append(((runs[i][1] - runs[i - 1][2]) / freq * 1000, (runs[i + 1][1] - runs[i][2]) / freq * 1000, runs[i][3]))
if gaps:
    print(f"repeated short states: {len(gaps)}; reads in them {Counter(g[2] for g in gaps)}; "
          f"gap from the previous read median {sorted(g[0] for g in gaps)[len(gaps) // 2]:.2f} ms, to the next {sorted(g[1] for g in gaps)[len(gaps) // 2]:.2f} ms")
