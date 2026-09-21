#!/usr/bin/env python3
"""Host-side checks for E11.

  compare.py writes <gfx plan or run log> [<the next call's log> ...] [--whole]
      The driver's register writes ("W <offset> <value>" lines of bc250kmd_cli gfx) against amdgpu's writes on unit A
      in the same steps (E03 trace, the windows of driver/kmd/gen_regs.py's Gfx sequence), in order. A difference is
      "address" when the register carries a memory address by its name (ring, MQD, EOP, write-back, clear-state
      buffer: ours live elsewhere, README), otherwise OTHER. The TLB flushes after the driver's GART binds are taken
      out first and checked by themselves: amdgpu made the same ones, but at allocation time (0.2495 to 0.2528 s),
      not in this step. This is a second opinion, independent of the host replay
      test in driver/shim/test: it knows nothing about the code, only names and the trace.

  compare.py state --control <sweep> [...] --after <sweep> [...]
      As in E10: registers that moved, against Linux's values after init.

Logs from the target are UTF-16 (Windows PowerShell); both encodings are accepted.
"""

import argparse
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "trace"))
from extract_phase import accesses  # noqa: E402
from summarize_init import register_names  # noqa: E402

_spec = importlib.util.spec_from_file_location("e10_compare", ROOT / "experiments/E10-psp-firmware-load/compare.py")
_e10 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_e10)
text, cmd_state = _e10.text, _e10.cmd_state

E03 = ROOT / "evidence/linux/2026-09-21-E03-init-trace"
# as in driver/kmd/gen_regs.py
STEP = (r"^(GC\.(?!GCVM_|GCMC_)|GC\.GCMC_VM_CACHEABLE_DRAM_ADDRESS_END$|NBIO\.(RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN|BIF_SDMA[01]_DOORBELL_RANGE|"
        r"BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_(BASE_LOW|BASE_HIGH|CNTL))$)")
# The interrupt block at 1.56 s is not part of this experiment (README).
WINDOWS = [(0.0375, 0.0385), (0.5496, 0.551)]
# amdgpu_gart_invalidate_tlb(): what amdgpu wrote after each of its binds (E03 trace, 0.2495 to 0.2528 s, all 66 writes)
FLUSH = {"GC.GCVM_INVALIDATE_ENG17_REQ": 0x00F80001, "MMHUB.MMVM_INVALIDATE_ENG17_REQ": 0x00F80001,
         "MMHUB.MMVM_INVALIDATE_ENG17_SEM": 0}
WRITE = re.compile(r"^\s*W 0x([0-9A-Fa-f]+) ([0-9A-Fa-f]{8})\s*$")
STAGE = re.compile(r"^\s*S (\d+) .*? rc (-?\d+) first write (\d+)")
ADDRESS = re.compile(r"(_BASE(_HI|_LO)?$|_BASE_ADDR|_ADDR(_HI|_LO|_H|_L)?$|_ADDR_(HI|LO)$|MQD_BASE|EOP_BASE|RPTR_REPORT|WPTR_POLL|CSIB_ADDR|"
                     r"RB_RPTR_ADDR|RB_WPTR_POLL_ADDR|IB_BASE)")


def cmd_writes(args):
    names = register_names()
    lines = [line for log in args.log for line in text(log).splitlines()]      # several calls of one run, in order
    every = [(int(m.group(1), 16), int(m.group(2), 16)) for m in map(WRITE.match, lines) if m]
    flushes = [(o, v) for o, v in every if names.get(o) in FLUSH]
    wrong = [(o, v) for o, v in flushes if FLUSH[names[o]] != v]
    ours = [(o, v) for o, v in every if names.get(o) not in FLUSH]
    stages = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in map(STAGE.match, lines) if m]
    trace = [(off, val, name) for since, until in WINDOWS
             for _, _, name, off, val in accesses(E03 / "amdgpu-events.txt", STEP, since=since, until=until, names=names)]
    print(f"driver: {len(ours)} register writes in {len(stages)} stages   trace: {len(trace)} writes in the step")
    print(f"TLB flush writes after binds: {len(flushes)}, with a value other than amdgpu's: {len(wrong)}")
    for o, v in wrong:
        print(f"  flush    OTHER   driver 0x{o:05X} {v:08X} {names[o]}")
    for stage, rc, first in stages:
        print(f"  stage {stage}: rc {rc}, writes from [{first}]")
    same = address = 0
    other = len(wrong)
    for i in range(max(len(ours), len(trace)) if args.whole else len(ours)):
        o = ours[i] if i < len(ours) else None
        t = trace[i] if i < len(trace) else None
        if o and t and o == t[:2]:
            same += 1
            continue
        name = names.get(o[0], "?") if o else ""
        if o and t and o[0] == t[0] and ADDRESS.search(name):
            address += 1
            kind = "address"
        else:
            other += 1
            kind = "OTHER  "
        print(f"  [{i:4}] {kind} driver {o and f'0x{o[0]:05X} {o[1]:08X} {name}'}   trace {t and f'{t[2]} 0x{t[0]:05X} {t[1]:08X}'}")
    print(f"equal {same}, address registers with another value {address}, other differences {other}")
    print(f"verdict: {'AS EXPECTED' if other == 0 else 'NOT AS EXPECTED'}")
    return 0 if other == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("writes")
    w.add_argument("log", nargs="+")
    w.add_argument("--whole", action="store_true", help="the log is the whole step: trace writes beyond its end count as missing")
    w.set_defaults(func=cmd_writes)
    s = sub.add_parser("state")
    s.add_argument("--control", nargs="+", required=True)
    s.add_argument("--after", nargs="+", required=True)
    s.set_defaults(func=cmd_state)
    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
