"""Run 1 facts for the report: countdown steps per case, every window flash with its kind and the box of its text
against the window's rectangle, and the overlay rows around the window's start and end (the console question)."""
import csv
import re
import sys
from pathlib import Path

from PIL import Image

d = Path(sys.argv[1])


def text_box(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    pts = [(x, y) for y in range(h) for x in range(w) if px[x, y][1] > 120 and px[x, y][2] > 120 and px[x, y][0] < 160]
    if not pts:
        return f"{w}x{h}, no cyan"
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return f"{w}x{h}, text x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)}"


for case in "abc":
    log = (d / f"{case}-log.txt").read_text().splitlines()
    freq = float(re.search(r"freq=(\d+)", next(l for l in log if l.startswith("start "))).group(1))
    win = next(l for l in log if l.startswith("target window "))
    zero = int(re.search(r"qpc=(\d+)", win).group(1))
    rect = re.search(r"rect=(\S+)", win).group(1)
    rows = list(csv.DictReader(open(d / f"{case}-frames.csv")))
    w = [r for r in rows if r["target"] == "window"]
    seq = []
    for r in w:
        t = int(r["present_qpc"] or r["qpc"])
        if seq and seq[-1][0] == r["hash"]:
            continue
        seq.append((r["hash"], t, int(r["navy"]), int(r["cyan"])))
    regular = [a for a, b in zip(seq, seq[1:]) if (b[1] - a[1]) / freq >= 0.3]
    print(f"case {case}: window rect {rect}; regular contents {len(regular)} (+ the last one)")
    if case == "a":
        print(f"   ref {text_box(d / 'a-ref.bmp')}")
    for l in log:
        if l.startswith("flash target=window"):
            q = int(re.search(r" qpc=(\d+)", l).group(1))
            bmp = re.search(r"bmp=(\S+)", l).group(1)
            kind = re.search(r"kind=(\S+)", l).group(1)
            ms = re.search(r" ms=(\S+)", l).group(1)
            dirty = next((r for r in w if int(r["present_qpc"] or r["qpc"]) == q), None)
            dr = f"dirty {dirty['dx0']},{dirty['dy0']},{dirty['dx1']},{dirty['dy1']}" if dirty else "dirty ?"
            box = text_box(d / bmp) if (d / bmp).exists() else "no bmp"
            h = re.search(r" hash=(\S+)", l).group(1)
            print(f"   +{(q - zero) / freq:7.3f} s {ms:>5} ms {kind:16s} {dr}; {box}; hash {h}")
    # Overlay: rows of the overlay target around the window's appearance and end (its black count).
    ov = [r for r in rows if r["target"] == "overlay"]
    if ov:
        def burst(lo, hi):
            sel = [r for r in ov if lo <= (int(r["present_qpc"] or r["qpc"]) - zero) / freq <= hi]
            if not sel:
                return "none"
            t = [(int(r["present_qpc"] or r["qpc"]) - zero) / freq for r in sel]
            b = [int(r["black"]) for r in sel]
            return f"{len(sel)} rows +{t[0]:.3f}..+{t[-1]:.3f} s, black {min(b)}..{max(b)} (first {b[0]}, last {b[-1]})"
        print(f"   overlay at the start (-2..+1 s): {burst(-2, 1)}")
        last = (int(w[-1]['present_qpc'] or w[-1]['qpc']) - zero) / freq
        print(f"   overlay at the end ({last - 1:.1f}..{last + 1.5:.1f} s): {burst(last - 1, last + 1.5)}")
