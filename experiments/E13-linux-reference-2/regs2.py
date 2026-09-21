#!/usr/bin/env python3
"""Read named registers through amdgpu's own debugfs interface (amdgpu_regs2), on the probe, while amdgpu runs.

    python3 -u regs2.py lists.json state        global registers, no selection
    python3 -u regs2.py lists.json hqd          the per-queue registers for every (me, pipe, queue) of the list

amdgpu does the SRBM selection itself (amdgpu_gfx_select_me_pipe_q under its srbm_mutex) and puts it back after every
read, so a live CP is never looked at through somebody else's selection. Read-only: nothing is ever written through
this file. One line per register, written before and completed after the read, as sweep_stream.py does.
"""

import fcntl
import glob
import json
import os
import struct
import sys
import time


def iowr(kind, number, size):
    return (3 << 30) | (size << 16) | (kind << 8) | number


SET_STATE = iowr(0x20, 0, 40)       # AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE, struct amdgpu_debugfs_regs2_iocdata (10 x u32)


def select(fd, srbm=None):
    me, pipe, queue = srbm if srbm else (0, 0, 0)
    fcntl.ioctl(fd, SET_STATE, struct.pack("10I", 1 if srbm else 0, 0, 0, 0, 0, 0, me, pipe, queue, 0))


def read(fd, name, offset, prefix=""):
    sys.stdout.write(f"{prefix}{name} 0x{offset:05x} ")
    sys.stdout.flush()
    time.sleep(0.002)
    try:
        value = struct.unpack("<I", os.pread(fd, 4, offset))[0]
        sys.stdout.write(f"{value:08x}\n")
    except OSError as e:
        sys.stdout.write(f"ERROR {e.errno}\n")
    sys.stdout.flush()


def main():
    lists = json.load(open(sys.argv[1]))
    what = sys.argv[2]
    paths = glob.glob("/sys/kernel/debug/dri/*/amdgpu_regs2")
    if not paths:
        sys.exit("no amdgpu_regs2: debugfs not mounted or amdgpu not loaded")
    fd = os.open(paths[0], os.O_RDONLY)
    print(f"# {paths[0]} {what} {time.strftime('%H:%M:%S')}")
    if what == "state":
        select(fd)
        for name, offset in lists["state"]:
            read(fd, name, offset)
    elif what == "hqd":
        for me, pipe, queue in lists["queues"]:
            select(fd, (me, pipe, queue))
            for name, offset in lists["hqd"]:
                read(fd, name, offset, f"me{me}.pipe{pipe}.queue{queue} ")
        select(fd)
    else:
        sys.exit("state | hqd")
    os.close(fd)


if __name__ == "__main__":
    main()
