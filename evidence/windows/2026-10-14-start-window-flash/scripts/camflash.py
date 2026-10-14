"""Does the camera see the DDA flashes? Map lab QPC to UTC, align the camera to the window's appear/disappear edges,
and look at the green level (the cyan text) of the window's cells around each DDA flash and around every tick."""
import csv
import re
import sys
from pathlib import Path

import numpy as np

cam_path, lab = sys.argv[1], Path(sys.argv[2])
rows = list(csv.reader(open(cam_path)))[1:]
ct = np.array([float(r[1]) for r in rows])
v = np.array([[float(x) for x in r[2:]] for r in rows]).reshape(len(rows), -1, 3)
COLS = 30
n = v.shape[1]
blue = v[:, :, 2] - (v[:, :, 0] + v[:, :, 1]) / 2
green = v[:, :, 1]


def lab_utc(case):
    log = (lab / f"{case}-log.txt").read_text().splitlines()
    st = next(l for l in log if l.startswith("start "))
    q0 = int(re.search(r"qpc=(\d+)", st).group(1))
    freq = float(re.search(r"freq=(\d+)", st).group(1))
    ft = int(re.search(r"filetime=(\d+)", st).group(1))
    base = ft / 1e7 - 11644473600.0
    conv = lambda q: base + (q - q0) / freq
    win = [r for r in csv.DictReader(open(lab / f"{case}-frames.csv")) if r["target"] == "window"]
    navy = [r for r in win if int(r["navy"] or 0) * 2 > 488 * 39]
    appear = conv(int(navy[0]["present_qpc"] or navy[0]["qpc"]))
    gone = next((r for r in win if r is not navy[0] and int(r["navy"] or 0) * 2 <= 488 * 39 and
                 int(r["frame"]) > int(navy[-1]["frame"]) - 50), None)
    leave = conv(int(navy[-1]["present_qpc"] or navy[-1]["qpc"]))
    flashes = []
    for l in log:
        if l.startswith("flash target=window"):
            q = int(re.search(r" qpc=(\d+)", l).group(1))
            flashes.append((conv(q), re.search(r"kind=(\S+)", l).group(1), float(re.search(r" ms=([\d.]+)", l).group(1))))
    # The ticks: every change of the window content while it is the window.
    ticks = []
    last = None
    for r in navy:
        if r["hash"] != last:
            ticks.append(conv(int(r["present_qpc"] or r["qpc"])))
            last = r["hash"]
    return appear, leave, flashes, ticks


# Window cells: blue excess much higher while case a's window is up than before it.
cases = {c: lab_utc(c) for c in "abc"}
# Camera edges: the mean blue excess of the cells that move most.
rng = blue.max(axis=0) - blue.min(axis=0)
# Rows 4 and 5 of the 30x8 grid, columns 2..17: the window per the range map.
cells = [r * COLS + c for r in (4, 5) for c in range(2, 18)]
s = blue[:, cells].mean(axis=1)
base = np.median(s[:100])
up = s > base + 0.5 * (np.percentile(s, 95) - base)
edges = np.flatnonzero(np.diff(up.astype(int)))
print("camera edges:", [(round(ct[e + 1], 3), "up" if up[e + 1] else "down") for e in edges])
for c, (a, l, f, t) in cases.items():
    print(f"case {c}: lab appear {a:.3f} leave {l:.3f} flashes {len(f)} ticks {len(t)}")
# Offset: camera up edges minus lab appear times, in order.
ups = [ct[e + 1] for e in edges if up[e + 1]]
downs = [ct[e + 1] for e in edges if not up[e + 1]]
offs = []
for k, c in enumerate("abc"):
    if k < len(ups):
        offs.append(ups[k] - cases[c][0])
    if k < len(downs):
        offs.append(downs[k] - cases[c][1])
print("offsets camera-lab:", [round(o, 3) for o in offs])
off = float(np.median(offs))
print(f"offset used {off:.3f} s")

# Text cells: per window period, the cells whose green varies most (the countdown digits and the text).
text_cells = [r * COLS + c for r in (4, 5) for c in range(2, 18)]
g = green[:, text_cells].mean(axis=1)
dg = np.abs(np.diff(g))
for c, (a, l, f, t) in cases.items():
    m = (ct > a + off + 0.5) & (ct < l + off - 0.5)
    idx = np.flatnonzero(m)
    if not len(idx):
        continue
    gm = g[idx]
    # A one-frame outlier: differs from both neighbours in the same direction, neighbours agree.
    out = []
    for i in idx[1:-1]:
        d1, d2 = g[i] - g[i - 1], g[i] - g[i + 1]
        if d1 * d2 > 0 and min(abs(d1), abs(d2)) > 3 * np.median(dg[idx[:-1]]) + 0.3:
            out.append((ct[i] - off, g[i] - (g[i - 1] + g[i + 1]) / 2))
    print(f"case {c}: green mean {gm.mean():.2f} sd {gm.std():.2f}; one-frame outliers {len(out)}")
    for tt, d in out:
        near = min(f, key=lambda x: abs(x[0] - tt)) if f else None
        nt = min(t, key=lambda x: abs(x - tt))
        print(f"   outlier at lab {tt:.3f} (+{tt - a:.3f} s) dgreen {d:+.2f}; nearest tick {tt - nt:+.3f} s"
              + (f"; nearest DDA flash {tt - near[0]:+.3f} s ({near[1]})" if near else ""))
    for fl in f:
        j = np.argmin(np.abs(ct - off - fl[0]))
        seg = g[max(0, j - 3): j + 4]
        print(f"   DDA flash +{fl[0] - a:.3f} s {fl[1]} {fl[2]} ms: camera green around it "
              + " ".join(f"{x:.1f}" for x in seg))
