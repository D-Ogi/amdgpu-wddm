#!/usr/bin/env python3
"""Host-side checks for E10.

  compare.py commands <psp plan or load log> --firmware <dir> [--expect planned|loaded]
      Register writes against amdgpu's PSP mailbox traffic on unit A (E03), and the PSP commands against what the
      firmware files' own headers give (amdgpu_ucode_init_single_fw's rules, written down a second time here, in
      Python, independently of driver/shim/bc250_psp.c). With --expect loaded: every command has its fence, status 0,
      and a TMR address inside the TMR.

  compare.py state --control <sweep> <sweep> [...] --after <sweep> [...]
      Sweeps through the independent witness (bc250rd). Every register that differs from the control outside the
      noise set is listed with the value Linux shows after amdgpu's init (E03), and counted as "as Linux" or "other".

Logs from the target are UTF-16 (Windows PowerShell); both encodings are accepted.
"""

import argparse
import importlib.util
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "trace"))
from extract_phase import accesses  # noqa: E402
from summarize_init import register_names  # noqa: E402

# E09's helpers (sweep parsing, the named clocks); loaded by path because that file is called compare.py as well.
_spec = importlib.util.spec_from_file_location("e09_compare", ROOT / "experiments/E09-gart-enable/compare.py")
_e09 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_e09)
VOLATILE, sweep, text = _e09.VOLATILE, _e09.sweep, _e09.text

E03 = ROOT / "evidence/linux/2026-09-21-E03-init-trace"
STEP = r"^MP0\.MP0_SMN_C2PMSG_\d+$"         # as in driver/kmd/gen_regs.py
UNTIL = 0.309
WRITE = re.compile(r"^\s*W 0x([0-9A-Fa-f]+) ([0-9A-Fa-f]{8})\s*$")
COMMAND = re.compile(r"^\s*C\s+(\d+) id (\d+) type\s+(\d+) size\s+(\d+) mc 0x([0-9A-Fa-f]+) (submitted|planned)\s+rc (-?\d+) "
                     r"status 0x([0-9A-Fa-f]+) us (\d+) tmr 0x([0-9A-Fa-f]+)")
TMR = re.compile(r"tmr\s+MC 0x([0-9A-Fa-f]+), physical 0x([0-9A-Fa-f]+)")
TMR_SIZE = 0x400000

# (name, file, GFX_FW_TYPE_*) in AMDGPU_UCODE_ID order, psp_get_fw_type()
IMAGES = [("SDMA0", "sdma", 9), ("SDMA1", "sdma1", 10), ("CP_CE", "ce", 3), ("CP_PFP", "pfp", 2), ("CP_ME", "me", 1),
          ("CP_MEC1", "mec", 4), ("CP_MEC1_JT", "mec", 5), ("CP_MEC2", "mec2", 4), ("CP_MEC2_JT", "mec2", 6), ("RLC_G", "rlc", 8)]


def expected_sizes(firmware):
    sizes = []
    for name, stem, _ in IMAGES:
        data = (Path(firmware) / f"cyan_skillfish2_{stem}.bin").read_bytes()
        ucode_size = struct.unpack_from("<I", data, 20)[0]
        jt_size = struct.unpack_from("<I", data, 40)[0]        # gfx_firmware_header_v1_0.jt_size
        if name.endswith("_JT"):
            sizes.append(jt_size * 4)
        elif name.startswith("CP_MEC"):
            sizes.append(ucode_size - jt_size * 4)
        else:
            sizes.append(ucode_size)
    return sizes


def cmd_commands(args):
    names = register_names()
    lines = text(args.log).splitlines()
    ours = [(int(m.group(1), 16), int(m.group(2), 16)) for m in map(WRITE.match, lines) if m]
    commands = [m.groups() for m in map(COMMAND.match, lines) if m]
    tmr = next((m for m in map(TMR.search, lines) if m), None)
    trace = [(off, val, name) for _, _, name, off, val in accesses(E03 / "amdgpu-events.txt", STEP, until=UNTIL, names=names)]
    want = trace if args.expect == "loaded" else trace[:4]
    bad = 0
    print(f"driver: {len(ours)} register writes   trace: {len(trace)} in the step, {len(want)} expected here")
    for i in range(max(len(ours), len(want))):
        o = ours[i] if i < len(ours) else None
        t = want[i] if i < len(want) else None
        if o is None or t is None or o[0] != t[0] or o[1] != t[1]:
            bad += 1
            print(f"  [{i}] driver {o and f'0x{o[0]:05X} {o[1]:08X}'}   trace {t and f'{t[2]} 0x{t[0]:05X} {t[1]:08X}'}")
    print(f"register writes: {'MATCH' if bad == 0 else 'MISMATCH'}")

    sizes = expected_sizes(args.firmware)
    if len(commands) != 1 + len(IMAGES):
        print(f"commands: {len(commands)}, expected {1 + len(IMAGES)}")
        bad += 1
    tmr_mc = int(tmr.group(1), 16) if tmr else 0
    previous_end = 0
    for n, cid, ftype, size, mc, how, rc, status, us, tmr_addr in commands:
        n, cid, ftype, size, rc, us = int(n), int(cid), int(ftype), int(size), int(rc), int(us)
        mc, status, tmr_addr = int(mc, 16), int(status, 16), int(tmr_addr, 16)
        problems = []
        if n == 1:
            label = "SETUP_TMR"
            if cid != 5 or size != TMR_SIZE or mc != tmr_mc or mc % TMR_SIZE:
                problems.append("contents")
        else:
            name, _, want_type = IMAGES[n - 2]
            label = name
            if cid != 6 or ftype != want_type:
                problems.append(f"type {ftype}, amdgpu sends {want_type}")
            if size != sizes[n - 2]:
                problems.append(f"size {size}, the file header gives {sizes[n - 2]}")
            if mc % 0x1000 or mc < previous_end:
                problems.append("placement")
            previous_end = mc + size
            if args.expect == "loaded" and not (tmr_mc <= tmr_addr < tmr_mc + TMR_SIZE):
                problems.append(f"TMR address 0x{tmr_addr:X} outside the TMR")
        if args.expect == "loaded" and (how != "submitted" or rc != 0 or status != 0):
            problems.append(f"{how}, rc {rc}, status 0x{status:X}")
        if args.expect == "planned" and how != "planned":
            problems.append("a plan must not submit")
        bad += len(problems)
        print(f"  [{n:2}] {label:<11} {size:7} bytes  {us:6} us  tmr 0x{tmr_addr:X}  " + ("; ".join(problems) if problems else "ok"))
    if args.expect == "loaded":
        # amdgpu's own spacing on unit A, from the trace: time between consecutive write pointer updates
        stamps = [t for t, _, name, _, _ in accesses(E03 / "amdgpu-events.txt", r"C2PMSG_67$", until=UNTIL, names=names)]
        stamps.append(stamps[-1])
        print("  amdgpu's spacing between submissions on unit A, ms: " +
              " ".join(f"{(b - a) * 1000:.1f}" for a, b in zip(stamps, stamps[1:-1])) + "  (the last one is not in the trace)")
    print(f"verdict: {'AS EXPECTED' if bad == 0 else 'NOT AS EXPECTED'} ({args.expect})")
    return 0 if bad == 0 else 1


def cmd_state(args):
    controls = [sweep([p]) for p in args.control]
    after = sweep(args.after)
    linux_after = sweep([E03 / "sweep-after-init.log"])
    reference, noise = {}, set()
    for c in controls:
        for off, (name, val) in c.items():
            if off in reference and reference[off][1] != val:
                noise.add(off)
            reference.setdefault(off, (name, val))
    as_linux = other = 0
    for off, (name, val) in sorted(after.items()):
        if off in noise or off not in reference or reference[off][1] == val:
            continue
        if VOLATILE.match(name):
            print(f"  clock/counter {name}: {reference[off][1]} -> {val}")
            continue
        linux = linux_after.get(off, ("", "not swept"))[1]
        same = linux == val
        as_linux += same
        other += not same
        print(f"  {'as Linux after init' if same else 'OTHER              '} {name}: {reference[off][1]} -> {val}" +
              ("" if same else f"   (Linux after init: {linux})"))
    for name in ("GC.GCVM_L2_PROTECTION_FAULT_STATUS", "MMHUB.MMVM_L2_PROTECTION_FAULT_STATUS"):
        hits = [v for o, (n, v) in after.items() if n == name]
        print(f"  {name} = {hits[0] if hits else 'not swept'}")
    print(f"noise set: {len(noise)} registers; changed: {as_linux} to Linux's value after init, {other} to something else")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("commands")
    c.add_argument("log")
    c.add_argument("--firmware", required=True)
    c.add_argument("--expect", choices=["planned", "loaded"], default="planned")
    c.set_defaults(func=cmd_commands)
    s = sub.add_parser("state")
    s.add_argument("--control", nargs="+", required=True)
    s.add_argument("--after", nargs="+", required=True)
    s.set_defaults(func=cmd_state)
    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
