#!/usr/bin/env python3
"""Host-side checks for E09.

  compare.py plan  <gart-plan-or-enable log>
      The writes the driver planned (or made) against amdgpu's trace of the same step on unit A: same offsets,
      same order, same values, except the registers that carry addresses of our own pages.

  compare.py state <gart enable log> --control <sweep> <sweep> [...] --after <sweep> [...] [--expect written|firmware]
      Sweeps through the independent witness (bc250rd). Control sweeps: two or more per IP, taken with nothing
      changed; they give the firmware's values and the noise set. --expect written: every register of the step
      holds what was written (or what Linux's sweep after init holds, for self-clearing bits) and nothing else
      moved. --expect firmware: everything reads as in the control.

Logs from the target are UTF-16 (Windows PowerShell); both encodings are accepted.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "trace"))
from extract_phase import accesses  # noqa: E402
from summarize_init import register_names  # noqa: E402

E03 = ROOT / "evidence/linux/2026-09-21-E03-init-trace"
STEP = r"^(GC\.(GCVM|GCMC)|MMHUB\.(MMVM|MMMC))"        # as in driver/kmd/gen_regs.py
UNTIL = 0.26
SWEEP = re.compile(r"^([A-Z0-9]+\.\S+) (0x[0-9a-fA-F]+) ([0-9A-Fa-f]{8})\s*$")
WRITE = re.compile(r"^\s*W 0x([0-9A-Fa-f]+) ([0-9A-Fa-f]{8})\s*$")
PROTOCOL = re.compile(r"_INVALIDATE_ENG\d+_(SEM|REQ|ACK)$")
# Free-running clocks and counters: every noise set so far (E05, E07, E08) consists of these. A control pair can
# miss one that ticks only once a minute, so they are named here, reported, and not counted as a change.
VOLATILE = re.compile(r"^GC\.(CP_DMA_READ_TAGS|RLC_GPU_CLOCK_32|RLC_REFCLOCK_TIMESTAMP_(LSB|MSB)|SQ_TIME_(HI|LO))$")


def text(path):
    raw = Path(path).read_bytes()
    return raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8", errors="replace")


def sweep(paths):
    values = {}
    for p in paths:
        for line in text(p).splitlines():
            m = SWEEP.match(line.strip())
            if m:
                values.setdefault(int(m.group(2), 16), (m.group(1), m.group(3).upper()))
    return values


def gart_log(path):
    writes, info = [], {}
    for line in text(path).splitlines():
        m = WRITE.match(line)
        if m:
            writes.append((int(m.group(1), 16), int(m.group(2), 16)))
        for key, pattern in (("dummy", r"dummy page\s+physical 0x([0-9A-Fa-f]+)"), ("scratch_mc", r"scratch\s+MC 0x([0-9A-Fa-f]+)"),
                             ("table", r"table\s+physical 0x([0-9A-Fa-f]+), MC 0x([0-9A-Fa-f]+)")):
            m = re.search(pattern, line)
            if m:
                info[key] = [int(g, 16) for g in m.groups()]
    return writes, info


def own_address_registers(info, names):
    """Registers whose value is an address of our pages, with the value we expect there."""
    firmware = sweep([E03 / "sweep-before-run1-GC-complete-then-hang.log", E03 / "sweep-before-run2-nonGC.log"])
    by_name = {name: off for off, (name, _) in firmware.items()}
    fb_offset = int(firmware[by_name["GC.GCMC_VM_FB_OFFSET"]][1], 16) << 24
    fb_base = (int(firmware[by_name["GC.GCMC_VM_FB_LOCATION_BASE"]][1], 16) & 0xFFFFFF) << 24
    scratch_physical = info["scratch_mc"][0] - fb_base + fb_offset        # amdgpu_gmc_vram_mc2pa
    expected = {}
    for off, name in names.items():
        if name.endswith("_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB"):
            expected[off] = (scratch_physical >> 12) & 0xFFFFFFFF
        elif name.endswith("_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB"):
            expected[off] = scratch_physical >> 44
        elif name.endswith("_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32"):
            expected[off] = (info["dummy"][0] >> 12) & 0xFFFFFFFF
        elif name.endswith("_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32"):
            expected[off] = info["dummy"][0] >> 44
    return expected


def cmd_plan(args):
    names = register_names()
    ours, info = gart_log(args.log)
    trace = [(off, val, name) for _, _, name, off, val in accesses(E03 / "amdgpu-events.txt", STEP, until=UNTIL, names=names)]
    own = own_address_registers(info, names)
    print(f"driver: {len(ours)} writes   trace: {len(trace)} writes in the step")
    bad = differ_own = 0
    for i, (off, val) in enumerate(ours):
        if i >= len(trace):
            print(f"  [{i}] 0x{off:05X} = {val:08X}   beyond the end of the trace")
            bad += 1
            continue
        t_off, t_val, t_name = trace[i]
        if off != t_off:
            print(f"  [{i}] ORDER/OFFSET: driver 0x{off:05X} ({names.get(off, '?')}), trace 0x{t_off:05X} ({t_name})")
            bad += 1
        elif val != t_val:
            if off in own and val == own[off]:
                differ_own += 1
                print(f"  [{i}] {t_name}: {val:08X} instead of amdgpu's {t_val:08X}: our own page, as computed")
            else:
                print(f"  [{i}] VALUE: {t_name} driver {val:08X}, trace {t_val:08X}" + (f", expected {own[off]:08X}" if off in own else ""))
                bad += 1
    rest = trace[len(ours):]
    foreign = [n for _, _, n in rest if not PROTOCOL.search(n)]
    print(f"trace writes after the compared prefix: {len(rest)}, of which not invalidation protocol: {len(foreign)}")
    print(f"verdict: {'MATCH' if bad == 0 and not foreign else 'MISMATCH'} ({bad} unexplained, {differ_own} own-address values)")
    return 0 if bad == 0 and not foreign else 1


def cmd_state(args):
    names = register_names()
    writes, info = gart_log(args.log)
    own = own_address_registers(info, names)
    written = {}
    for off, val in writes:
        written[off] = val
    controls = [sweep([p]) for p in args.control]
    after = sweep(args.after)
    linux_after = sweep([E03 / "sweep-after-init.log"])
    # control sweeps come in pairs or more per IP: the first value seen is the reference, any disagreement is noise
    reference, noise = {}, set()
    for c in controls:
        for off, (name, val) in c.items():
            if off in reference and reference[off][1] != val:
                noise.add(off)
            reference.setdefault(off, (name, val))

    bad = 0
    counts = {"as written": 0, "as Linux after init": 0, "own address": 0, "as firmware": 0, "not swept": 0}
    step = {off for off in written if not PROTOCOL.search(names.get(off, ""))}
    for off in sorted(step):
        name = names.get(off, f"?0x{off:05X}")
        if off not in after:
            counts["not swept"] += 1
            continue
        now = int(after[off][1], 16)
        if args.expect == "firmware":
            if off in reference and now == int(reference[off][1], 16):
                counts["as firmware"] += 1
            else:
                bad += 1
                print(f"  NOT RESTORED {name}: {now:08X}, firmware had {reference.get(off, ('', '?'))[1]}")
            continue
        expected = own.get(off, written[off])
        if now == expected:
            counts["own address" if off in own else "as written"] += 1
        elif off in linux_after and now == int(linux_after[off][1], 16):
            counts["as Linux after init"] += 1
            print(f"  note {name}: wrote {written[off]:08X}, reads {now:08X}, and so it does under Linux after init")
        else:
            bad += 1
            print(f"  UNEXPECTED {name}: wrote {written[off]:08X}, reads {now:08X}"
                  + (f", Linux after init {linux_after[off][1]}" if off in linux_after else ""))
    outside = residue = 0
    for off, (name, val) in sorted(after.items()):
        if off in step or off in noise or off not in reference:
            continue
        if reference[off][1] == val:
            continue
        if VOLATILE.match(name):
            print(f"  clock/counter {name}: {reference[off][1]} -> {val}")
        elif PROTOCOL.search(name):
            residue += args.expect == "firmware"
            print(f"  invalidation protocol {name}: {reference[off][1]} -> {val}"
                  + (f" (Linux after init: {linux_after[off][1]})" if off in linux_after else ""))
        elif off in linux_after and linux_after[off][1] == val:
            residue += args.expect == "firmware"
            print(f"  hardware response {name}: {reference[off][1]} -> {val}, the value Linux shows after init")
        else:
            outside += 1
            print(f"  OUTSIDE THE STEP {name}: {reference[off][1]} -> {val}")
    for name in ("GC.GCVM_L2_PROTECTION_FAULT_STATUS", "MMHUB.MMVM_L2_PROTECTION_FAULT_STATUS"):
        hits = [v for o, (n, v) in after.items() if n == name]
        print(f"  {name} = {hits[0] if hits else 'not swept'}")
        if hits and int(hits[0], 16) != 0:
            bad += 1
    print(f"step registers: {len(step)}  " + "  ".join(f"{k}: {v}" for k, v in counts.items() if v))
    print(f"noise set: {len(noise)} registers; changed outside the step and outside noise: {outside}")
    if residue:
        print(f"residue: {residue} registers keep the record that an invalidation ran (last request, acknowledge, SDMA's "
              f"invalidation status); they cannot be written back; the lines above show them next to the values Linux has after init")
    print(f"verdict: {'AS EXPECTED' if bad == 0 and outside == 0 else 'NOT AS EXPECTED'} (expect {args.expect})")
    return 0 if bad == 0 and outside == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("log")
    p.set_defaults(func=cmd_plan)
    s = sub.add_parser("state")
    s.add_argument("log")
    s.add_argument("--control", nargs="+", required=True)
    s.add_argument("--after", nargs="+", required=True)
    s.add_argument("--expect", choices=["written", "firmware"], default="written")
    s.set_defaults(func=cmd_state)
    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
