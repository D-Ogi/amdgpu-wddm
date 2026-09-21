#!/usr/bin/env python3
"""Read-only register sweep that reports over stdout (SSH) as it goes, so a hang names its cause.

    ssh root@probe python3 -u /media/usb/bc250/sweep_stream.py sweep.json [--skip FILE] [--order GC,MMHUB,...] > log

For every register one line is written in two steps: "<ip>.<name> <offset> " before the read, the
value after it. If the machine dies on a read, the last, unfinished line is the register that did it.
A short pause before each read lets sshd push the text out first. Nothing is ever written to the GPU.
Only named registers are read: on this SoC a read of the wrong BAR5 address hangs the machine.
--wait FLAG: map the BAR now (no driver bound yet), start reading when the file FLAG appears.
--skip FILE: regular expressions (one per line) of "<ip>.<name>" entries that must not be read.
"""

import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diag  # noqa: E402

DEV = "/sys/bus/pci/devices/0000:01:00.0"
PAUSE_S = 0.004


def main():
    args = sys.argv[1:]
    spec_path = args.pop(0)
    wait_for = None
    skip, order = [], ["GC", "MMHUB", "OSSSYS", "HDP", "NBIO", "MP0", "MP1"]
    while args:
        opt = args.pop(0)
        if opt == "--skip":
            with open(args.pop(0)) as f:
                skip = [re.compile(l.strip()) for l in f if l.strip() and not l.startswith("#")]
        elif opt == "--order":
            order = args.pop(0).split(",")
        elif opt == "--wait":
            wait_for = args.pop(0)
    with open(spec_path) as f:
        spec = json.load(f)
    items = []
    for ip in order:
        items += [(f"{ip}.{name}", off) for i, name, off in spec["regs"] if i == ip]
    # Offsets produced by the prior-art addressing rules are NOT read: most of them are not registers
    # at all, and reading an unused BAR5 address can hang this SoC (measured). They are resolved
    # offline against the named registers instead.
    bar = diag.Bar(DEV, 5, readonly=True)
    if wait_for:
        # The kernel refuses a new sysfs mmap once a driver owns the BAR: map first, sweep later.
        sys.stdout.write(f"# BAR mapped, waiting for {wait_for}\n")
        sys.stdout.flush()
        while not os.path.exists(wait_for):
            time.sleep(0.5)
    driver = os.path.basename(os.path.realpath(DEV + "/driver")) if os.path.exists(DEV + "/driver") else ""
    out = sys.stdout
    out.write(f"# sweep_stream kernel={os.uname().release} driver='{driver}' items={len(items)} time={time.strftime('%FT%TZ', time.gmtime())}\n")
    for key, off in items:
        if any(rx.search(key) for rx in skip):
            out.write(f"{key} {off:#07x} SKIPPED\n")
            continue
        out.write(f"{key} {off:#07x} ")
        out.flush()
        time.sleep(PAUSE_S)
        value = bar.r32(off)
        out.write("None\n" if value is None else f"{value:08X}\n")
    out.write("# complete\n")
    out.flush()


if __name__ == "__main__":
    main()
