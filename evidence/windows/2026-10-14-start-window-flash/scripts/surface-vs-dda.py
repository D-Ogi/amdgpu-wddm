"""Run 2 of the start-window flash: the window's own surface (GetDC reads, about 1 kHz) against DWM's frames (DDA).

    python surface-vs-dda.py <work dir> [case]

For each DDA flash of the window: the surface reads around it (count, largest gap), and whether the surface showed
the same picture (hash) or a similar one (counts) near it. Then the self-check (do the regular contents agree), the
surface flashes that DWM never composed, and the flash rate against the text changes."""
import bisect
import csv
import re
import sys
from pathlib import Path

d = Path(sys.argv[1])
case = sys.argv[2] if len(sys.argv) > 2 else "a"
log = (d / f"{case}-log.txt").read_text().splitlines()
freq = float(re.search(r"freq=(\d+)", next(l for l in log if l.startswith("start "))).group(1))
win = next(l for l in log if l.startswith("target window "))
zero = int(re.search(r"qpc=(\d+)", win).group(1))
sec = lambda q: (q - zero) / freq


def field(line, key):
    m = re.search(r"(?:^| )" + re.escape(key) + r"=(\S+)", line)
    return m.group(1) if m else None


frames = list(csv.DictReader(open(d / f"{case}-frames.csv")))
dda = [r for r in frames if r["target"] == "window"]
surf_rows = [r for r in frames if r["target"] == "surface"]
reads_path = d / f"{case}-surface.csv"
reads_q, reads_h = [], []
if reads_path.exists():
    for r in csv.DictReader(open(reads_path)):
        reads_q.append(int(r["qpc0"]))
        reads_h.append(r["hash"])
print(f"case {case}: DDA window rows {len(dda)}, surface content changes {len(surf_rows)}, surface reads {len(reads_q)}")
if reads_q:
    gaps = sorted((b - a) * 1000 / freq for a, b in zip(reads_q, reads_q[1:]))
    print(f"  read gaps ms: median {gaps[len(gaps) // 2]:.2f} p99 {gaps[int(len(gaps) * 0.99)]:.2f} max {gaps[-1]:.2f}")

# Self-check: the regular contents (those that stayed 300 ms or more in DDA) and whether the surface read them too.
def runs(rows, key):
    out = []
    for r in rows:
        t = int(r[key] or r["qpc"])
        if out and out[-1]["hash"] == r["hash"]:
            continue
        out.append({"hash": r["hash"], "t": t, "n": (int(r["navy"] or 0), int(r["cyan"] or 0))})
    for a, b in zip(out, out[1:]):
        a["stay"] = (b["t"] - a["t"]) * 1000 / freq
    if out:
        out[-1]["stay"] = None
    return out


dda_runs = runs(dda, "present_qpc")
surf_hashes = set(reads_h) | {r["hash"] for r in surf_rows}
regular = [r for r in dda_runs if r["stay"] and r["stay"] >= 300]
same = sum(r["hash"] in surf_hashes for r in regular)
print(f"  self-check: {same} of {len(regular)} regular DDA contents also read from the surface (same hash)")
if surf_rows:
    s = surf_rows[len(surf_rows) // 2]
    print(f"  a surface content: navy {s['navy']} cyan {s['cyan']} black {s['black']} other {s['other']}")

flashes = [l for l in log if l.startswith("flash target=window")]
sflashes = [l for l in log if l.startswith("flash target=surface")]
changes = len([r for r in dda_runs if r["n"][0] * 2 > 488 * 39])
print(f"  DDA window flashes {len(flashes)} over {changes} contents; surface flashes {len(sflashes)}")
for f in flashes:
    p = int(field(f, "qpc"))
    ms = float(field(f, "ms"))
    h = field(f, "hash")
    lo, hi = p - int(0.050 * freq), p + int(ms / 1000 * freq)
    i, j = bisect.bisect_left(reads_q, lo), bisect.bisect_right(reads_q, hi)
    window_reads = reads_q[i:j]
    g = max(((b - a) * 1000 / freq for a, b in zip(window_reads, window_reads[1:])), default=None)
    seen_same = any(reads_h[k] == h for k in range(max(0, i - 200), min(len(reads_h), j + 200)))
    near = sorted(sflashes, key=lambda x: abs(int(field(x, "qpc")) - p))[:1]
    ns = ""
    if near:
        dq = (int(field(near[0], "qpc")) - p) * 1000 / freq
        ns = f"; nearest surface flash {dq:+.1f} ms {field(near[0], 'kind')} cyan {field(near[0], 'cyan')} hash {'=' if field(near[0], 'hash') == h else '!='}"
    print(f"  DDA flash +{sec(p):.3f} s {ms} ms {field(f, 'kind')} cyan {field(f, 'cyan')}: {len(window_reads)} reads in "
          f"[-50 ms, +{ms} ms], largest gap {g if g is None else round(g, 2)} ms; same picture read from the surface "
          f"within 200 reads: {'yes' if seen_same else 'no'}{ns}")
for f in sflashes:
    q = int(field(f, "qpc"))
    near = min((abs(int(field(x, 'qpc')) - q) for x in flashes), default=None)
    print(f"  surface flash +{sec(q):.3f} s {field(f, 'ms')} ms {field(f, 'kind')} navy {field(f, 'navy')} cyan "
          f"{field(f, 'cyan')} bmp {field(f, 'bmp')}; nearest DDA flash "
          f"{'none' if near is None else f'{near * 1000 / freq:.1f} ms'}")
