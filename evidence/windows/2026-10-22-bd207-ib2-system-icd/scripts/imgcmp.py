"""Compare two screenshots of the same scene: size, PSNR, mean and 99.9th percentile absolute difference per
channel (0-255), and the share of pixels whose largest channel difference is above a threshold.

    python -I imgcmp.py <a> <b> [--threshold 16]

Exit 0 always when both images load; the numbers are the result. Exit 2 when the sizes differ.
"""
import argparse
import math
import sys

import numpy as np
from PIL import Image


def load(path):
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.float64)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--threshold", type=float, default=16.0)
    p.add_argument("--box", default="", help="x0,y0,x1,y1: compare only this rectangle (pixels, end exclusive)")
    args = p.parse_args()
    a, b = load(args.a), load(args.b)
    if args.box:
        x0, y0, x1, y1 = (int(v) for v in args.box.split(","))
        a, b = a[y0:y1, x0:x1], b[y0:y1, x0:x1]
        print(f"box {x0},{y0},{x1},{y1}")
    print(f"a {args.a} {a.shape[1]}x{a.shape[0]}")
    print(f"b {args.b} {b.shape[1]}x{b.shape[0]}")
    if a.shape != b.shape:
        print("sizes differ")
        return 2
    d = np.abs(a - b)
    mse = float(np.mean(d ** 2))
    psnr = float("inf") if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
    worst = d.max(axis=2)
    print(f"identical {bool(mse == 0)}")
    print(f"psnr_db {psnr:.2f}")
    print(f"mean_abs {float(d.mean()):.3f}")
    print(f"p99.9_abs {float(np.percentile(worst, 99.9)):.1f}")
    print(f"max_abs {float(worst.max()):.0f}")
    print(f"share_above_{args.threshold:g} {float((worst > args.threshold).mean()):.5f}")
    print(f"mean_luma_a {float(a.mean()):.2f} mean_luma_b {float(b.mean()):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
