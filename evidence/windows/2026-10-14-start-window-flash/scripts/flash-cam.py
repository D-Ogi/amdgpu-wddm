"""Camera witness of the start-window flash (2026-10-14, task of the lead): what the LG screen of unit A actually
shows over the start window, 30 frames a second, as numbers only.

One bounded ffmpeg run on the lab camera (DirectShow, MJPEG 1920x1080 at 30 fps). The crop is applied inside
ffmpeg, so no uncropped frame exists anywhere; the cropped frames go through a pipe into this process and are reduced
to the mean colour of 10x10-pixel cells at once. No frame is written to disk, so there is no video to delete.

    python flash-cam.py --seconds 170 --out <csv> [--box x,y,w,h]   (camera pixels; default: the start window's
                                                                       place on the LG, top-left of its 2026-10-13 box)
    python flash-cam.py --analyse <csv> [--window-cells ...]

The CSV: frame, t_utc (time.time() when the frame arrived here), then R,G,B means per cell, row by row. --analyse
finds the cells that change most (the window's text), and lists the frames that differ from both neighbours while
the neighbours agree: a picture that the screen showed for one camera frame (33 ms) or less.
"""
import argparse
import csv
import subprocess
import sys
import time

import numpy as np

CAMERA = "video=HD 1080P  PC-Camera"
# The LG box of snap.sh (fractions 0.351:0.339:0.377:0.256 of 1920x1080) starts at about (724, 276); the start window
# sits in its top-left corner. A generous box around it, checked on one cropped frame before the run.
DEFAULT_BOX = (724, 276, 340, 110)
CELL = 10


def record(args):
    x, y, w, h = args.box
    w -= w % CELL
    h -= h % CELL
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "dshow", "-video_size", "1920x1080",
           "-framerate", "30", "-vcodec", "mjpeg", "-i", CAMERA, "-t", str(args.seconds),
           "-vf", f"crop={w}:{h}:{x}:{y}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    size = w * h * 3
    cols, rows = w // CELL, h // CELL
    started = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    n = 0
    with open(args.out, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["frame", "t_utc"] + [f"c{r}_{c}_{k}" for r in range(rows) for c in range(cols) for k in "rgb"])
        deadline = started + args.seconds + 30
        while time.time() < deadline:
            buf = proc.stdout.read(size)
            if len(buf) < size:
                break
            t = time.time()
            a = np.frombuffer(buf, dtype=np.uint8).reshape(rows, CELL, cols, CELL, 3).astype(np.float32)
            m = a.mean(axis=(1, 3))
            out.writerow([n, f"{t:.4f}"] + [f"{v:.1f}" for v in m.reshape(-1)])
            n += 1
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    err = proc.stderr.read().decode("utf-8", "replace").strip()
    print(f"frames={n} seconds={time.time() - started:.1f} box={x},{y},{w},{h} grid={cols}x{rows} "
          f"ffmpeg_exit={proc.returncode}{' error=' + err if err else ''}")
    return 0 if n else 1


def analyse(args):
    with open(args.analyse, newline="") as f:
        r = csv.reader(f)
        head = next(r)
        rows = [list(map(float, row)) for row in r]
    a = np.array(rows)
    t = a[:, 1]
    cells = a[:, 2:].reshape(len(a), -1, 3)
    luma = cells @ np.array([0.299, 0.587, 0.114])
    # The cells of the window: those whose brightness moves the most over the run (text that changes each second).
    spread = luma.std(axis=0)
    pick = np.argsort(spread)[-args.cells:]
    v = luma[:, pick]
    d_prev = np.abs(v[1:-1] - v[:-2]).mean(axis=1)
    d_next = np.abs(v[1:-1] - v[2:]).mean(axis=1)
    d_span = np.abs(v[2:] - v[:-2]).mean(axis=1)
    noise = np.median(np.abs(np.diff(v, axis=0)).mean(axis=1))
    print(f"frames={len(a)} span={t[-1] - t[0]:.1f}s cells={len(pick)} noise={noise:.2f} "
          f"fps={len(a) / max(1e-6, t[-1] - t[0]):.1f}")
    hits = np.where((d_prev > args.k * noise) & (d_next > args.k * noise) & (d_span < 0.5 * np.minimum(d_prev, d_next)))[0] + 1
    for i in hits:
        print(f"single-frame at frame={int(a[i, 0])} t_utc={t[i]:.3f} prev={d_prev[i - 1]:.1f} next={d_next[i - 1]:.1f} "
              f"span={d_span[i - 1]:.1f} mean_luma={v[i].mean():.1f} neighbours={v[i - 1].mean():.1f}/{v[i + 1].mean():.1f}")
    # Large steps of the window cells: the window appearing and closing in each case, for the clock alignment.
    step = np.abs(np.diff(v.mean(axis=1)))
    for i in np.where(step > args.big)[0]:
        print(f"step at frame={int(a[i + 1, 0])} t_utc={t[i + 1]:.3f} from={v[i].mean():.1f} to={v[i + 1].mean():.1f}")
    return 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seconds", type=int, default=170)
    p.add_argument("--out")
    p.add_argument("--box", type=lambda s: tuple(int(v) for v in s.split(",")), default=DEFAULT_BOX)
    p.add_argument("--analyse")
    p.add_argument("--cells", type=int, default=40)
    p.add_argument("--k", type=float, default=6.0)
    p.add_argument("--big", type=float, default=20.0)
    args = p.parse_args()
    if args.analyse:
        return analyse(args)
    if not args.out or args.seconds > 175:
        sys.exit("give --out, and at most 175 s")
    return record(args)


if __name__ == "__main__":
    sys.exit(main())
