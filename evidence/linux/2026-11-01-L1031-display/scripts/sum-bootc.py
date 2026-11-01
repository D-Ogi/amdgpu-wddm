"""Summaries of the Boot C single-pipe display runs (D2 underflow, D4 INUSE, D6b flip states).
Usage: python sum-bootc.py <disp-out dir>"""
import collections
import gzip
import os
import sys

D = sys.argv[1]


def rows(lines):
    cols = None
    for line in lines:
        if line.startswith("H "):
            cols = line.split()[3:]
            if "|" in cols:
                cols.remove("|")
            continue
        if not line.startswith("C ") or cols is None:
            continue
        p = line.replace("|", " ").split()
        if len(p) < 3 + len(cols):
            continue
        yield dict(zip(cols, (int(v, 16) for v in p[3:3 + len(cols)])))


def comments(lines):
    return [line for line in lines if line.startswith("#") and "dcnsample" not in line]


l22 = gzip.open(os.path.join(D, "l22-underflow.txt.gz"), "rt", errors="replace").read().splitlines()
uf = collections.Counter()
n = 0
for r in rows(l22):
    n += 1
    uf[tuple(f"{k}={r[k] & 0x70000000:#x}" for k in r if k.endswith("DCHUBP_CNTL"))] += 1
print("D2 rows", n, "HUBP_UNDERFLOW_STATUS (mask 0x70000000):", dict(uf))
for c in comments(l22):
    print("D2", c[:160])
for line in l22:
    if not line.startswith(("#", "H ")) and not line[:1].isdigit():
        print("D2 text", line[:160])

l29 = open(os.path.join(D, "l29-inuse.txt"), errors="replace").read().splitlines()
seen = collections.Counter()
for r in rows(l29):
    seen[tuple(f"{k.split('_', 1)[1]}={r[k]:#x}" for k in r if "INUSE" in k or "PRIMARY_SURFACE_ADDRESS" in k)] += 1
print("D4 distinct states", len(seen))
for k, v in seen.most_common(8):
    print("D4", v, " ".join(k))

l48 = open(os.path.join(D, "l48b-timing.txt"), errors="replace").read().splitlines()
st = collections.Counter()
for r in rows(l48):
    st[tuple(f"{k}={r[k]:#x}" for k in r if "FLIP_CONTROL" in k or "MASTER_UPDATE_LOCK" in k)] += 1
print("D6b distinct states", len(st))
for k, v in st.most_common(8):
    print("D6b", v, " ".join(k))
for c in comments(l48)[:12]:
    print("D6b", c[:160])
