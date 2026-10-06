#!/usr/bin/env python3
"""H3 of E17: every entry pt_walk.py read out of VRAM equals the value amdgpu's tracepoint described.

    python experiments/E17-linux-reference-3/offline/check_walk_vs_trace.py \
        <amdgpu-events-hold.txt> <hold/pt-walk.txt> --vram-base 0x270000000 --mc-base 0xF400000000 \
        [--until <trace seconds>]

`amdgpu_vm_set_ptes` traces the arguments it hands to the SDMA packet, and the engine writes
`(addr + i*incr) | flags` at `pe + 8*i` (`ref/linux-src/.../amdgpu_vm_sdma.c`). The tracepoint's `pe` is
an MC address (VRAM at `--mc-base`, the "VRAM:" line of dmesg), while pt_walk.py prints the system
physical address of the table page (`--vram-base` + VRAM offset, facts M85). This replays every
write up to `--until` (the holder unmaps its buffers at the end of the phase, and those writes must
not count) and compares the last value written at each walked address with the value read.
"""

import argparse
import re
import sys

SET_PTES = re.compile(r"\s([\d.]+): amdgpu_vm_set_ptes: pe=([0-9a-f]+), addr=([0-9a-f]+), incr=(\d+), "
                      r"flags=([0-9a-f]+), count=(\d+)")
WALKED = re.compile(r"@ vram 0x([0-9a-f]+): ([0-9a-f]{16})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("walk")
    ap.add_argument("--vram-base", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--mc-base", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--until", type=float, default=None, help="ignore trace events after this timestamp")
    a = ap.parse_args()

    written = {}
    events = 0
    for line in open(a.trace, encoding="utf-8", errors="replace"):
        m = SET_PTES.search(line)
        if not m:
            continue
        if a.until is not None and float(m[1]) > a.until:
            break
        pe, addr, incr, flags, count = (int(m[2], 16), int(m[3], 16), int(m[4]), int(m[5], 16), int(m[6]))
        events += 1
        for i in range(count):
            written[pe + 8 * i] = (addr + i * incr) | flags

    same = differ = 0
    seen = set()
    for line in open(a.walk, encoding="utf-8", errors="replace"):
        m = WALKED.search(line)
        if not m:
            continue
        where, value = int(m[1], 16), int(m[2], 16)
        pe = where - a.vram_base + a.mc_base
        want = written.get(pe)
        if want == value:
            same += 1
        else:
            differ += 1
            shown = "no write" if want is None else f"{want:016x}"
            print(f"MISMATCH vram {where:#x} (pe {pe:#x}): read {value:016x}, trace {shown}")
        seen.add(where)
    print(f"{events} set_ptes events replayed; {same + differ} walked entries ({len(seen)} distinct): "
          f"{same} equal the traced value, {differ} differ")
    return 1 if differ or not same else 0


if __name__ == "__main__":
    sys.exit(main())
