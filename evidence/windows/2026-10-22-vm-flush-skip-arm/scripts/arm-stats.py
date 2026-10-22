"""Frame statistics of the VM flush arm runs (host side).

Reads arm/<label>/benchmarkResults/frames.csv (the game's own per-frame times of the benchmark) for each label and
prints one line per run: frames, average FPS (frames / sum of frame times, the same quantity as the game's
averageFps), the 1 % and 0.1 % low FPS (from the 99th and 99.9th percentile frame time), and the average FPS of
consecutive time windows from the benchmark start. The late windows run at a lower clock when the KMD thermal policy
steps down (84-85 C), so the early windows compare the KMD images at the same clock.

    python arm-stats.py r0 a1 a2 b1 b2 [--window 10]
"""
import csv
import os
import statistics
import sys

ARM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "arm")


def frame_times(label):
    path = os.path.join(ARM, label, "benchmarkResults", "frames.csv")
    times = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = csv.reader(f)
        next(rows)
        for row in rows:
            if len(row) >= 2 and row[1].strip():
                times.append(float(row[1]))
    return times


def percentile(values, q):
    ordered = sorted(values)
    k = (len(ordered) - 1) * q
    lo = int(k)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def windows(times, seconds):
    out, acc, n = [], 0.0, 0
    for t in times:
        acc += t
        n += 1
        if acc >= seconds * 1000.0:
            out.append(n * 1000.0 / acc)
            acc, n = 0.0, 0
    if n:
        out.append(n * 1000.0 / acc)
    return out


def main(argv):
    window = 10
    if "--window" in argv:
        at = argv.index("--window")
        window = int(argv[at + 1])
        argv = argv[:at] + argv[at + 2:]
    for label in argv:
        try:
            t = frame_times(label)
        except OSError as e:
            print(f"{label}: no frames ({e})")
            continue
        total = sum(t)
        avg = len(t) * 1000.0 / total
        low1 = 1000.0 / percentile(t, 0.99)
        low01 = 1000.0 / percentile(t, 0.999)
        w = windows(t, window)
        print(f"{label}: {len(t)} frames in {total / 1000.0:.1f} s, avg {avg:.2f} FPS, 1% low {low1:.2f}, "
              f"0.1% low {low01:.2f}, median frame {statistics.median(t):.2f} ms")
        print(f"{label}: {window} s windows FPS " + " ".join(f"{x:.2f}" for x in w))


if __name__ == "__main__":
    main(sys.argv[1:])
