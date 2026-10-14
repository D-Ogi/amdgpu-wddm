"""Per run: the changes from one full text to the next in the surface reads, and how many of them had the partial
picture (hash 69b00b13dfdbc8af) in between. The last read of a run (the window closes) is left out.

    python change-count.py <work dir>...
"""
import csv
import gzip
import sys
from pathlib import Path

PARTIAL = "69b00b13dfdbc8af"
for d in map(Path, sys.argv[1:]):
    p = d / "c-surface.csv"
    rows = list(csv.DictReader(gzip.open(p.with_suffix(".csv.gz"), "rt") if not p.exists() else open(p)))[:-1]
    runs = []
    for r in rows:
        if not runs or runs[-1] != r["hash"]:
            runs.append(r["hash"])
    full = [h for h in runs if h != PARTIAL]
    changes = with_partial = 0
    for i in range(1, len(runs)):
        if runs[i] != PARTIAL:
            j = i - 1
            between = False
            while j >= 0 and runs[j] == PARTIAL:
                between = True
                j -= 1
            if j >= 0:
                changes += 1
                with_partial += between
    other = sorted({h for h in runs if h != PARTIAL and runs.count(h) > 1})
    print(f"{d.name}: {len(rows)} reads, {len(full)} full texts, {changes} changes, {with_partial} with the partial picture between, "
          f"partial before the first full text: {runs[0] == PARTIAL}, full texts seen twice: {len(other)}")
