#!/usr/bin/env python3
"""List, in order, the register writes (and optionally reads) of one part of amdgpu's init trace, by name.

summarize_init.py gives the big picture; this gives the exact recipe of one step, which is what a driver
milestone is written from. Consecutive identical accesses are folded into one line with a count.

    python tools/trace/extract_phase.py <amdgpu-events.txt> --match 'GCVM|GCMC|MMVM|MMMC' [--reads] [--until 0.26]

Times are seconds since the first register access. Output goes to stdout. `accesses()` is the same thing for
other scripts (driver/kmd/gen_regs.py builds the write table of a kernel command from it).
"""

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from summarize_init import EVENT, register_names  # noqa: E402


def accesses(events, match, reads=False, since=0.0, until=1e9, names=None):
    """Yield (seconds, 'W'|'R', name, byte offset, value) for every access whose register name matches."""
    names = names or register_names()
    want = re.compile(match)
    t0 = None
    for line in Path(events).read_text(encoding="utf-8", errors="replace").splitlines():
        m = EVENT.search(line)
        if not m:
            continue
        t, kind, reg, val = float(m.group(1)), m.group(2), int(m.group(3), 16), int(m.group(4), 16)
        if t0 is None:
            t0 = t
        t -= t0
        if t < since or t > until or (kind == "rreg" and not reads):
            continue
        off = reg * 4
        name = names.get(off, f"?0x{off:05x}")
        if want.search(name):
            yield t, "W" if kind == "wreg" else "R", name, off, val


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events")
    ap.add_argument("--match", required=True, help="regex on the register name")
    ap.add_argument("--reads", action="store_true", help="include reads")
    ap.add_argument("--since", type=float, default=0.0)
    ap.add_argument("--until", type=float, default=1e9)
    args = ap.parse_args()

    last, count = None, 0

    def flush():
        if last is not None:
            t, kind, name, off, val = last
            print(f"{t:8.3f}  {kind}  {name:<44} 0x{off:05x}  {val:08X}" + (f"   x{count}" if count > 1 else ""))

    for access in accesses(args.events, args.match, args.reads, args.since, args.until):
        if last is not None and last[1:] == access[1:]:
            count += 1
            continue
        flush()
        last, count = access, 1
    flush()


if __name__ == "__main__":
    main()
