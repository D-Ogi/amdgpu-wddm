#!/usr/bin/env python3
"""Read-only register reads on unit A under Linux. Each line is written in two steps: "<name> <offset> " before
the read, the value after it, so a hang leaves the address that caused it as the last, unfinished line.

    regstream.py debugfs <table> [label]   through amdgpu's debugfs amdgpu_regs (amdgpu loaded)
    regstream.py raw <table> [label]       through the sysfs resource5 mmap (no driver needed)

Table lines: '<name> 0x<BAR5 byte offset>', generated on the development PC with tools/regcalc.
Nothing is written to the GPU.
"""
import glob
import mmap
import os
import struct
import sys
import time

DEV = "/sys/bus/pci/devices/0000:01:00.0"
PAUSE_S = 0.004


def entries(path):
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            yield parts[0], int(parts[1], 16)


def main():
    mode, table = sys.argv[1], sys.argv[2]
    label = sys.argv[3] if len(sys.argv) > 3 else ""
    out = sys.stdout
    if mode == "debugfs":
        path = sorted(glob.glob("/sys/kernel/debug/dri/*/amdgpu_regs"))[0]
        fd = os.open(path, os.O_RDONLY)
        read = lambda off: struct.unpack("<I", os.pread(fd, 4, off))[0]
    elif mode == "raw":
        path = DEV + "/resource5"
        fd = os.open(path, os.O_RDONLY | os.O_SYNC)
        size = os.fstat(fd).st_size
        mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
        mv = memoryview(mm).cast("I")
        read = lambda off: mv[off // 4]
    else:
        sys.exit(__doc__)
    out.write(f"# {label} mode={mode} {path} table={os.path.basename(table)} utc={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")
    n = 0
    for name, off in entries(table):
        out.write(f"{name} 0x{off:05X} ")
        out.flush()
        time.sleep(PAUSE_S)
        try:
            out.write(f"0x{read(off):08X}\n")
        except OSError as e:
            out.write(f"ERR {e}\n")
        n += 1
    out.write(f"# done {n} reads\n")
    out.flush()


if __name__ == "__main__":
    main()
