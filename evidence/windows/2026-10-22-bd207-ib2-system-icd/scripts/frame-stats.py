"""Frame statistics of the Vulkan route runs (host side).

Reads the frame log of the game benchmark (frames.csv: the per-frame times in ms in the second column) for each run
and prints one line per run: frames, average FPS (frames / sum of frame times, the same quantity as the averageFps of
the game), the 1 % and 0.1 % low FPS (from the 99th and 99.9th percentile frame time), and the average FPS of
consecutive 10 s windows from the benchmark start. The definitions are the same as in the arm-stats.py of the
2026-10-22 ring VM flush evidence.

    python frame-stats.py <label>=<frames.csv> ...
"""
import csv
import statistics
import sys


def frame_times(path):
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
    for arg in argv:
        label, path = arg.split("=", 1)
        t = frame_times(path)
        total = sum(t)
        avg = len(t) * 1000.0 / total
        low1 = 1000.0 / percentile(t, 0.99)
        low01 = 1000.0 / percentile(t, 0.999)
        print(f"{label}: {len(t)} frames in {total / 1000.0:.1f} s, avg {avg:.2f} FPS, 1% low {low1:.2f}, "
              f"0.1% low {low01:.2f}, median frame {statistics.median(t):.2f} ms")
        print(f"{label}: 10 s windows FPS " + " ".join(f"{x:.2f}" for x in windows(t, 10)))


if __name__ == "__main__":
    main(sys.argv[1:])
