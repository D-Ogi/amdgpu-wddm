#!/usr/bin/env python3
"""List, in order, the register writes (and optionally reads) of one part of amdgpu's init trace, by name.

summarize_init.py gives the big picture; this gives the exact recipe of one step, which is what a driver
milestone is written from. Consecutive identical accesses are folded into one line with a count.

    python tools/trace/extract_phase.py <amdgpu-events.txt> --match 'GCVM|GCMC|MMVM|MMMC' [--reads] [--until 0.26]

Times are seconds since the first register access. Output goes to stdout.
"""

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from summarize_init import EVENT, register_names  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events")
    ap.add_argument("--match", required=True, help="regex on the register name")
    ap.add_argument("--reads", action="store_true", help="include reads")
    ap.add_argument("--since", type=float, default=0.0)
    ap.add_argument("--until", type=float, default=1e9)
    args = ap.parse_args()

    names = register_names()
    want = re.compile(args.match)
    t0 = None
    last, count = None, 0

    def flush():
        if last is not None:
            t, kind, name, off, val = last
            print(f"{t:8.3f}  {kind}  {name:<44} 0x{off:05x}  {val:08X}" + (f"   x{count}" if count > 1 else ""))

    for line in Path(args.events).read_text(encoding="utf-8", errors="replace").splitlines():
        m = EVENT.search(line)
        if not m:
            continue
        t, kind, reg, val = float(m.group(1)), m.group(2), int(m.group(3), 16), int(m.group(4), 16)
        if t0 is None:
            t0 = t
        t -= t0
        if t < args.since or t > args.until or (kind == "rreg" and not args.reads):
            continue
        off = reg * 4
        name = names.get(off, f"?0x{off:05x}")
        if not want.search(name):
            continue
        key = ("W" if kind == "wreg" else "R", name, off, val)
        if last is not None and last[1:] == key:
            count += 1
            continue
        flush()
        last, count = (t,) + key, 1
    flush()


if __name__ == "__main__":
    main()
