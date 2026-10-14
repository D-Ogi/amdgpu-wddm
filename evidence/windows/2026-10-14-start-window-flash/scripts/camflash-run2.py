"""Run 2's camera numbers against the DDA flashes of case a. The window lowers the blue excess of the cells it covers
in this box, so the edges come from a drop, not a rise. Numbers only."""
import csv
import re
import sys
from pathlib import Path

import numpy as np

cam_path, lab = sys.argv[1], Path(sys.argv[2])
rows = list(csv.reader(open(cam_path)))[1:]
ct = np.array([float(r[1]) for r in rows])
v = np.array([[float(x) for x in r[2:]] for r in rows]).reshape(len(rows), -1, 3)
blue = v[:, :, 2] - (v[:, :, 0] + v[:, :, 1]) / 2
green = v[:, :, 1]
rng = blue.max(axis=0) - blue.min(axis=0)
top = np.argsort(rng)[-20:]
s = blue[:, top].mean(axis=1)
hi, lo = np.median(s[:100]), np.median(s[len(s) // 2 - 50: len(s) // 2 + 50])
on = s < (hi + lo) / 2
idx = np.flatnonzero(on)
cam_up, cam_down = ct[idx[0]], ct[idx[-1] + 1]
print(f"camera: blue excess off {hi:.1f}, on {lo:.1f}; window on from {cam_up:.3f} to {cam_down:.3f} ({cam_down - cam_up:.3f} s); "
      f"edges inside: {int(np.count_nonzero(np.diff(on[idx[0]:idx[-1] + 1].astype(int))))}")

log = (lab / "a-log.txt").read_text().splitlines()
st = next(l for l in log if l.startswith("start "))
q0 = int(re.search(r"qpc=(\d+)", st).group(1))
freq = float(re.search(r"freq=(\d+)", st).group(1))
base = int(re.search(r"filetime=(\d+)", st).group(1)) / 1e7 - 11644473600.0
conv = lambda q: base + (q - q0) / freq
win = [r for r in csv.DictReader(open(lab / "a-frames.csv")) if r["target"] == "window"]
navy = [r for r in win if int(r["navy"] or 0) * 2 > 488 * 39]
appear = conv(int(navy[0]["present_qpc"] or navy[0]["qpc"]))
leave = conv(int(navy[-1]["present_qpc"] or navy[-1]["qpc"]))
offs = (cam_up - appear, cam_down - leave)
off = offs[0]  # the appear edge; the lab's last navy frame is a countdown step before the close
print(f"lab: window from {appear:.3f} to {leave:.3f} ({leave - appear:.3f} s); camera - lab {offs[0]:+.3f} / {offs[1]:+.3f}, used {off:+.3f}")

# Text cells: inside the window period, the cells whose green varies most.
m = (ct > cam_up + 0.5) & (ct < cam_down - 0.5)
gr = np.abs(np.diff(green[m], axis=0)).mean(axis=0)  # frame-to-frame change, not the slow drift
cells = np.argsort(gr)[-12:]
g = green[:, cells].mean(axis=1)
w = np.flatnonzero(m)
dg = np.abs(np.diff(g[w]))
thr = 3 * np.median(dg) + 0.3
out = [i for i in w[1:-1] if (g[i] - g[i - 1]) * (g[i] - g[i + 1]) > 0 and min(abs(g[i] - g[i - 1]), abs(g[i] - g[i + 1])) > thr]
flashes = []
for l in log:
    if l.startswith("flash target=window"):
        q = int(re.search(r" qpc=(\d+)", l).group(1))
        flashes.append((conv(q), re.search(r"kind=(\S+)", l).group(1), re.search(r" cyan=(\d+)", l).group(1)))
print(f"camera one-frame outliers in the text cells (threshold {thr:.2f}): {len(out)}")
for i in out:
    t = ct[i] - off
    near = min(flashes, key=lambda f: abs(f[0] - t))
    print(f"   +{t - appear:8.3f} s dgreen {g[i] - (g[i - 1] + g[i + 1]) / 2:+6.2f}; nearest DDA flash {t - near[0]:+.3f} s ({near[1]})")
for f in flashes:
    j = int(np.argmin(np.abs(ct - off - f[0])))
    hit = any(abs(j - i) <= 1 for i in out)
    print(f"   DDA flash +{f[0] - appear:8.3f} s {f[1]:15s} cyan {f[2]:>4}: camera green " +
          " ".join(f"{x:.1f}" for x in g[max(0, j - 2): j + 3]) + ("  <- outlier" if hit else ""))
