#!/usr/bin/env python3
"""Compare a Windows bc250rd sweep with the Linux reference sweeps, register by register.

    python tools/win/bc250rd/compare_sweeps.py <windows evidence dir> [> comparison.txt]
"""

import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
REF = ROOT / "evidence" / "linux" / "2026-09-21-E03-init-trace"
LINE = re.compile(r"^([A-Z0-9]+)\.(\S+) (0x[0-9a-f]+) ([0-9A-F]{8})$")


def load(paths, only=None, exclude=None):
    out = {}
    for p in paths:
        for line in p.read_text(encoding="utf-8").splitlines():
            m = LINE.match(line)
            if m and (only is None or m.group(1) == only) and m.group(1) != exclude:
                out[f"{m.group(1)}.{m.group(2)}"] = (m.group(3), m.group(4))
    return out


def main():
    win = load(sorted(Path(sys.argv[1]).glob("sweep-[A-Z]*.log")))
    lin = load([REF / "sweep-before-run1-GC-complete-then-hang.log"], only="GC")
    lin.update(load([REF / "sweep-before-run2-nonGC.log"], exclude="GC"))
    after = load([REF / "sweep-after-init.log"])

    same = [k for k in win if k in lin and win[k][1] == lin[k][1]]
    diff = [k for k in win if k in lin and win[k][1] != lin[k][1]]
    print(f"registers read under Windows: {len(win)}; in the Linux pre-driver reference: {len(lin)}")
    print(f"equal: {len(same)}   different: {len(diff)}   missing on either side: {len(set(win) ^ set(lin))}")
    print("per block (equal/different):",
          ", ".join(f"{ip} {sum(k.startswith(ip + '.') for k in same)}/{n}"
                    for ip, n in sorted(Counter(k.split('.')[0] for k in diff).items())))
    like_after = [k for k in diff if k in after and after[k][1] == win[k][1]]
    print(f"of the different ones, equal to the Linux value AFTER amdgpu init: {len(like_after)}")
    print()
    print(f"{'register':58} {'offset':8} {'linux-pre':9} {'windows':9} {'linux-post':9}")
    for k in diff:
        print(f"{k:58} {win[k][0]:8} {lin[k][1]:9} {win[k][1]:9} {after.get(k, ('', '-'))[1]:9}")


if __name__ == "__main__":
    main()
