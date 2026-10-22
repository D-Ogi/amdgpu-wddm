"""GPU busy per frame of the VM flush arm runs over one benchmark span (host side).

Aligns arm/<label>/presentmon.csv to arm/<label>/benchmarkResults/frames.csv the way pmbins.py does (best match of
the first 300 frame times), then prints, over the benchmark seconds [start, end): frames, FPS, the mean PresentMon
MsGPUBusy per frame and the GPU busy share. The early span runs at the 2000 MHz ceiling in every run; the KMD
thermal policy steps the clock down from about 50 s into the benchmark.

    python arm-gpu.py <start s> <end s> r0 a1 a2 b1 b2
"""
import csv
import os
import sys

ARM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "arm")


def num(x):
    try:
        return float(x)
    except (ValueError, TypeError):
        return None


def main(argv):
    start, end = float(argv[0]), float(argv[1])
    for label in argv[2:]:
        d = os.path.join(ARM, label)
        with open(os.path.join(d, "benchmarkResults", "frames.csv"), newline="", encoding="utf-8-sig") as f:
            bench = [float(r[1]) for r in list(csv.reader(f))[1:] if len(r) > 1 and r[1].strip()]
        with open(os.path.join(d, "presentmon.csv"), newline="", encoding="utf-8-sig") as f:
            pm = [r for r in csv.DictReader(f) if r["Application"].lower().startswith("cyberpunk")]
        ft = [num(r["MsBetweenPresents"]) or 0.0 for r in pm]
        k = min(300, len(bench))
        best = min(range(0, len(ft) - len(bench) + 1), key=lambda o: sum(abs(ft[o + i] - bench[i]) for i in range(k)))
        t, frames, frame_ms, busy = 0.0, 0, 0.0, 0.0
        for r, f in zip(pm[best:best + len(bench)], bench):
            t += f / 1000.0
            if start <= t < end:
                g = num(r["MsGPUBusy"])
                if g is not None:
                    frames += 1
                    frame_ms += num(r["MsBetweenPresents"]) or 0.0
                    busy += g
        print(f"{label}: {start:.0f}-{end:.0f} s, {frames} frames, {1000.0 * frames / frame_ms:.2f} FPS, "
              f"GPU busy {busy / frames:.3f} ms per frame, share {busy / frame_ms:.3f}")


if __name__ == "__main__":
    main(sys.argv[1:])
