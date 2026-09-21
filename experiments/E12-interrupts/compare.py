#!/usr/bin/env python3
"""Host-side checks for E12.

  compare.py rerun <first gfx run log> <second gfx run log>
      The register writes of a second bring-up in the same boot against the first one's ("W <offset> <value>" lines of
      bc250kmd_cli gfx), as an edit script: what the second run wrote in addition, what it left out. TLB flush writes
      are compared by count only. The first run is compared with amdgpu's trace by E11's compare.py; this one says how
      far a bring-up over a GPU that was already brought up and torn down differs from a cold one.

  compare.py ih <ih plan or init log>
      The IH sequence's register writes against amdgpu's navi10_ih_irq_init() on unit A (E03 trace, 0.25282 to
      0.25290 s), in order. Address registers (the ring, the write-back slot, the dummy page) may differ, nothing else.
      The TLB flush after the ring's GART bind is taken out first and checked by value, as in E11.

  compare.py irq <gfx run log whose last stage is 8>
      Stage 8's writes against the trace's interrupt block (1.560 to 1.562 s, E11's register filter).

Logs from the target are UTF-16 (Windows PowerShell); both encodings are accepted.
"""

import argparse
import difflib
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("e11_compare", ROOT / "experiments/E11-gfx-bringup/compare.py")
_e11 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_e11)


def writes(log, names):
    every = [(int(m.group(1), 16), int(m.group(2), 16)) for m in map(_e11.WRITE.match, _e11.text(log).splitlines()) if m]
    return [w for w in every if names.get(w[0]) not in _e11.FLUSH], [w for w in every if names.get(w[0]) in _e11.FLUSH]


def cmd_rerun(args):
    names = _e11.register_names()
    first, first_flush = writes(args.first, names)
    second, second_flush = writes(args.second, names)
    print(f"first run: {len(first)} writes and {len(first_flush)} TLB flush writes   second run: {len(second)} and {len(second_flush)}")
    added = removed = 0
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=first, b=second, autojunk=False).get_opcodes():
        if tag == "equal":
            continue
        for o, v in first[i1:i2]:
            removed += 1
            print(f"  only first  [{i1:4}] 0x{o:05X} {v:08X} {names.get(o, '?')}")
        for o, v in second[j1:j2]:
            added += 1
            print(f"  only second [{j1:4}] 0x{o:05X} {v:08X} {names.get(o, '?')}")
    print(f"only in the first run {removed}, only in the second run {added}")
    return 0


IH_STEP = r"^(OSSSYS\.IH_|NBIO\.(INTERRUPT_CNTL2?|BIF_IH_DOORBELL_RANGE)$)"
ADDRESS = {"OSSSYS.IH_RB_BASE", "OSSSYS.IH_RB_BASE_HI", "OSSSYS.IH_RB_WPTR_ADDR_LO", "OSSSYS.IH_RB_WPTR_ADDR_HI",
           "NBIO.INTERRUPT_CNTL2", "NBIO.BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW",
           "NBIO.BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH"}


def against_trace(ours, flushes, step, window, names, drop_empty_selections=False):
    trace = [(off, val, name) for _, _, name, off, val in
             _e11.accesses(_e11.E03 / "amdgpu-events.txt", step, since=window[0], until=window[1], names=names)]
    if drop_empty_selections:
        # amdgpu selects a pipe and lets go of it again with no write in between (it only read there): nothing for a
        # write sequence to reproduce. Taken out of the trace side and counted.
        kept, i = [], 0
        while i < len(trace):
            if (i + 1 < len(trace) and trace[i][2] == "GC.GRBM_GFX_CNTL" and trace[i][1] != 0
                    and trace[i + 1][2] == "GC.GRBM_GFX_CNTL" and trace[i + 1][1] == 0):
                i += 2
                continue
            kept.append(trace[i])
            i += 1
        print(f"trace: {(len(trace) - len(kept)) // 2} selection(s) without a write left out")
        trace = kept
    wrong = [(o, v) for o, v in flushes if _e11.FLUSH[names[o]] != v]
    print(f"driver: {len(ours)} register writes   trace: {len(trace)} writes   TLB flush writes: {len(flushes)}, wrong value: {len(wrong)}")
    same = address = 0
    other = len(wrong)
    for i in range(max(len(ours), len(trace))):
        o = ours[i] if i < len(ours) else None
        t = trace[i] if i < len(trace) else None
        if o and t and o == t[:2]:
            same += 1
            continue
        if o and t and o[0] == t[0] and t[2] in ADDRESS:
            address += 1
            kind = "address"
        else:
            other += 1
            kind = "OTHER  "
        print(f"  [{i:3}] {kind} driver {o and f'0x{o[0]:05X} {o[1]:08X} {names.get(o[0])}'}   trace {t and f'{t[2]} 0x{t[0]:05X} {t[1]:08X}'}")
    print(f"equal {same}, address registers with another value {address}, other differences {other}")
    print(f"verdict: {'AS EXPECTED' if other == 0 else 'NOT AS EXPECTED'}")
    return 0 if other == 0 else 1


def cmd_ih(args):
    names = _e11.register_names()
    ours, flushes = writes(args.log, names)
    return against_trace(ours, flushes, IH_STEP, (0.25282, 0.25290), names)


def cmd_irq(args):
    names = _e11.register_names()
    lines = _e11.text(args.log).splitlines()
    first = [int(m.group(3)) for m in map(_e11.STAGE.match, lines) if m and int(m.group(1)) == 8]
    if not first:
        sys.exit("no stage 8 in this log")
    every = [(int(m.group(1), 16), int(m.group(2), 16)) for m in map(_e11.WRITE.match, lines) if m]
    ours = [w for w in every[first[0]:] if names.get(w[0]) not in _e11.FLUSH]
    return against_trace(ours, [], _e11.STEP, (1.560, 1.562), names, drop_empty_selections=True)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rerun")
    r.add_argument("first")
    r.add_argument("second")
    for name in ("ih", "irq"):
        sub.add_parser(name).add_argument("log")
    args = ap.parse_args()
    return {"rerun": cmd_rerun, "ih": cmd_ih, "irq": cmd_irq}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
