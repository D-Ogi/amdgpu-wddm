#!/usr/bin/env python3
"""Compare two tracefs captures of amdgpu's init (amdgpu_device_rreg / amdgpu_device_wreg events): E03's and E13's.

    python compare.py <E03 amdgpu-events.txt> <E13 amdgpu-events-load.txt>

Reports the number of accesses, the register writes as a sequence (offset only, then offset and value), and the writes
whose value differs at the same place of the sequence, grouped by register offset. Offsets are printed as the trace has
them (dword index); naming them is tools/regcalc's work, not this script's.
"""

import collections
import difflib
import re
import sys

EVENT = re.compile(r"\s(\d+\.\d+): amdgpu_device_(rreg|wreg): 0x[0-9a-f]+, (0x[0-9a-f]+), (0x[0-9a-f]+)")


def load(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = EVENT.search(line)
            if m:
                out.append((float(m.group(1)), m.group(2) == "wreg", int(m.group(3), 16), int(m.group(4), 16)))
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    for name, t in (("first", a), ("second", b)):
        writes = sum(1 for e in t if e[1])
        print(f"{name}: {len(t)} accesses, {writes} writes, {len(t) - writes} reads, "
              f"{t[-1][0] - t[0][0]:.3f} s from the first to the last")
    wa = [(e[2], e[3]) for e in a if e[1]]
    wb = [(e[2], e[3]) for e in b if e[1]]
    oa, ob = [w[0] for w in wa], [w[0] for w in wb]
    sm = difflib.SequenceMatcher(None, oa, ob, autojunk=False)
    same_place = value_differs = 0
    differs = collections.Counter()
    only_a, only_b = collections.Counter(), collections.Counter()
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for k in range(i2 - i1):
                same_place += 1
                if wa[i1 + k][1] != wb[j1 + k][1]:
                    value_differs += 1
                    differs[oa[i1 + k]] += 1
        else:
            only_a.update(oa[i1:i2])
            only_b.update(ob[j1:j2])
    print(f"writes aligned by offset: {same_place}; of those with a different value: {value_differs}")
    print(f"writes only in the first: {sum(only_a.values())}; only in the second: {sum(only_b.values())}")
    for title, c in (("value differs at", differs), ("only in the first", only_a), ("only in the second", only_b)):
        print(f"-- {title} ({len(c)} offsets), most frequent first")
        for offset, n in c.most_common(40):
            print(f"   0x{offset:05x} x{n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
