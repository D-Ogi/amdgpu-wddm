#!/usr/bin/env python3
"""E05, hypothesis H3: did the switch of display drivers change GPU registers?

    python compare.py <before> <before2> <after> [more sweeps ...]

Sweeps are `bc250rd_cli sweep` logs (UTF-16 when written by a PowerShell redirect, UTF-8 otherwise). The first
two are taken without any change in between and give the noise floor: registers that differ between them move
on their own (clocks, counters, status). Every later sweep is compared with the second one; a register counts
as changed by the event only if it is not in the noise set.
"""
import sys


def load(path):
    raw = open(path, "rb").read()
    text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")
    regs = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and not line.startswith("#") and parts[1].startswith("0x"):
            regs[parts[0]] = (parts[1], parts[2])
    return regs


def main(paths):
    if len(paths) < 3:
        sys.exit(__doc__)
    sweeps = [load(p) for p in paths]
    base, control = sweeps[0], sweeps[1]
    noise = {n for n in base if n in control and base[n][1] != control[n][1]}
    print(f"registers per sweep: {[len(s) for s in sweeps]}")
    print(f"noise floor ({paths[0]} vs {paths[1]}): {len(noise)} registers differ with nothing changed")
    for n in sorted(noise):
        print(f"  ~ {n:<44} {base[n][0]}  {base[n][1]} -> {control[n][1]}")
    for path, sweep in zip(paths[2:], sweeps[2:]):
        differ = [n for n in control if n in sweep and control[n][1] != sweep[n][1]]
        real = [n for n in differ if n not in noise]
        print(f"\n{path}: {len(differ)} differ from the control sweep, {len(real)} of them outside the noise set")
        for n in real:
            print(f"  ! {n:<44} {control[n][0]}  {control[n][1]} -> {sweep[n][1]}")
        unreadable = [n for n in control if n not in sweep]
        if unreadable:
            print(f"  missing in this sweep: {len(unreadable)}")


if __name__ == "__main__":
    main(sys.argv[1:])
