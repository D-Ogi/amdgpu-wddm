#!/usr/bin/env python3
"""Unit A, Linux, root: NCT6686D EC reference probe through /dev/port (fan Part B ground truth, DESIGN.md A13/B7).

read   : identity, present masks, RPM, duty read-back, 0xA00 mode, 0xA01 command, 0xCF8 status, monitor sources,
         the cost of one EC register read.
write  : channel FAN only (index 1 = Linux fan2, the fan that turns). Save 0xA00 and 0xA28+FAN, open the
         configuration phase, set the mode bit, duty 255 (full speed first), then 102 (40 %), then restore the
         saved duty and mode, close. Restore runs in finally. 1 Hz samples of RPM, duty and k10temp throughout.
Register facts: scratch/fan-control/facts-chip.md 6.3, 8.1, 8.2 (mainline nct6683.c, nct6687d). Base 0x0A20.
"""
import glob
import json
import os
import sys
import time

BASE = 0x0A20
PAGE, INDEX, DATA = BASE + 4, BASE + 5, BASE + 6
FAN = 1
MODE, CMD, STATUS = 0xA00, 0xA01, 0xCF8
CFG_PHASE, CFG_INVALID, CFG_CHECK_DONE, CFG_LOCK = 1 << 3, 1 << 4, 1 << 5, 1 << 6

fd = None


def outb(port, v):
    os.pwrite(fd, bytes([v & 0xFF]), port)


def inb(port):
    return os.pread(fd, 1, port)[0]


def rd(reg):
    outb(PAGE, 0xFF)
    outb(PAGE, reg >> 8)
    outb(INDEX, reg & 0xFF)
    return inb(DATA)


def wr(reg, v):
    outb(PAGE, 0xFF)
    outb(PAGE, reg >> 8)
    outb(INDEX, reg & 0xFF)
    outb(DATA, v)


def rpm(i):
    return (rd(0x140 + 2 * i) << 8) | rd(0x141 + 2 * i)


def k10temp():
    for d in glob.glob("/sys/class/hwmon/hwmon*"):
        try:
            if open(d + "/name").read().strip() == "k10temp":
                return int(open(d + "/temp1_input").read()) / 1000.0
        except OSError:
            pass
    return None


def log(**kv):
    kv["t"] = round(time.time(), 3)
    print(json.dumps(kv), flush=True)


def sample(phase, seconds):
    for _ in range(seconds):
        log(phase=phase, rpm=rpm(FAN), duty=rd(0x160 + FAN), a28=rd(0xA28 + FAN), mode=rd(MODE),
            status=rd(STATUS), cmd=rd(CMD), tctl=k10temp())
        time.sleep(1)


def wait_status(cond, what, tries=1000):
    for n in range(tries):
        s = rd(STATUS)
        if cond(s):
            return s, n
        time.sleep(0.001)
    raise RuntimeError(f"timeout waiting for {what}: status 0x{rd(STATUS):02x} cmd 0x{rd(CMD):02x}")


def cfg_open():
    s0 = rd(STATUS)
    s, n1 = wait_status(lambda s: not (s & CFG_PHASE) and not (rd(CMD) & 0x80), "phase clear")
    wr(CMD, 0x80)
    s, n2 = wait_status(lambda s: not (s & CFG_LOCK) and (s & CFG_PHASE), "open")
    log(step="open", status_before=s0, status_after=s, polls=[n1, n2])


def cfg_close():
    s0 = rd(STATUS)
    wr(CMD, 0x40)
    s, n = wait_status(lambda s: s & CFG_CHECK_DONE, "check done")
    log(step="close", status_before=s0, status_after=s, polls=n, invalid=bool(s & CFG_INVALID),
        locked=bool(s & CFG_LOCK))
    if s & CFG_INVALID:
        raise RuntimeError("EC rejected the configuration")


def read_all():
    ident = {"cust_id": [rd(0x602), rd(0x603)], "date": [rd(0x604), rd(0x605), rd(0x606)],
             "fw": [rd(0x608), rd(0x609)], "hwm_cfg_0x180": rd(0x180)}
    tach = [i for i in range(8) if rd(0x1C0 + i) & 0x80]
    outs = [i for i in range(8) if rd(0x1D0 + i) & 0x80]
    log(kind="identity", **ident, tach_present=tach, duty_present=outs)
    log(kind="fans", rpm=[rpm(i) for i in tach], duty=[rd(0x160 + i) for i in range(8)],
        a28=[rd(0xA28 + i) for i in range(8)], mode=rd(MODE), cmd=rd(CMD), status=rd(STATUS))
    srcs = [(i, rd(0x1A0 + i)) for i in range(32)]
    log(kind="monitors", src=[[i, s] for i, s in srcs if s & 0x7F],
        val=[[i, (rd(0x100 + 2 * i) << 8) | rd(0x101 + 2 * i)] for i, s in srcs if s & 0x7F])
    t0 = time.perf_counter()
    for _ in range(1000):
        rd(0x602)
    dt = (time.perf_counter() - t0) / 1000
    log(kind="timing", us_per_register_read=round(dt * 1e6, 2), port_ops_per_read=4)
    return outs


def write_test():
    if not (rd(0x1D0 + FAN) & 0x80):
        raise SystemExit("fan output %d not present" % FAN)
    sample("before", 5)
    cfg_open()
    saved_mode, saved_a28 = rd(MODE), rd(0xA28 + FAN)
    cfg_close()
    log(step="saved", mode=saved_mode, a28=saved_a28)
    try:
        for duty, secs in ((255, 10), (102, 15)):
            cfg_open()
            wr(MODE, rd(MODE) | (1 << FAN))
            wr(0xA28 + FAN, duty)
            log(step="set", duty=duty, readback=rd(0xA28 + FAN), mode=rd(MODE))
            cfg_close()
            sample(f"duty{duty}", secs)
    finally:
        cfg_open()
        wr(0xA28 + FAN, saved_a28)
        wr(MODE, saved_mode)
        log(step="restore", a28=rd(0xA28 + FAN), mode=rd(MODE))
        cfg_close()
        sample("after", 12)


if __name__ == "__main__":
    if any(l.startswith(("nct6683 ", "nct6687 ")) for l in open("/proc/modules")):
        raise SystemExit("an nct668x driver is loaded; unload it first (shared ports)")
    fd = os.open("/dev/port", os.O_RDWR)
    read_all()
    if "write" in sys.argv[1:]:
        write_test()
