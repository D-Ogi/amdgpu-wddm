#!/usr/bin/env python3
"""Display lane, lab side: sample named DCN registers through amdgpu's debugfs at a high rate, change log only.

One persistent process (E21's 20 Hz came from starting python per sample; a loop of pread calls here reaches
several kHz). Every sample reads all 'watch' and 'phase' registers; a line is written only when a watch value
changes, with the phase values (scan position, frame counter) of that same sample. Time stamps are
CLOCK_MONOTONIC ns, the clock of DRM events and of ftrace with trace_clock=mono.

Optional writes (R1 steps only; every write is logged and undone on exit, SIGTERM and SIGINT included):
  --set REG.FIELD=V      set a field at start, put the old field value back at the end. If the field reads back
                         different during the run, that is counted ('lost') and written again.
  --ack REG.OCC:CLEAR    count events of a sticky status field OCC: when OCC reads 1, write CLEAR=1 by
                         read-modify-write (the way DC's irq service acks), like a handler would.
  --clear-sticky REG.FIELD  write FIELD=1 once at start (a write-1-to-clear field, e.g. HUBP_UNDERFLOW_CLEAR).
Storm guard (when --set or --ack is given): the amdgpu line of /proc/interrupts is read every 100 ms; above
--irq-limit interrupts per second all --set fields are put back and the run ends with exit code 5.

Usage: dcnsample.py --secs S [--watch R,R..] [--phase R,R..] [--set ..] [--ack ..] [--clear-sticky ..]
                    [--max-hz N] [--irq-limit N] [--tag T]
Output lines:
  # header        H names of the columns
  C t_ns n w1 w2 .. | p1 p2 ..        a change of any watch value (n = sample index)
  A t_ns REG.OCC count value           an ack event
  W t_ns REG old new                   a write
  S t_ns n samples_per_s irq_per_s     once a second
  E ...                                end summary
"""
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcnregs  # noqa: E402


def irq_total():
    try:
        for line in open("/proc/interrupts"):
            if "amdgpu" in line:
                return sum(int(x) for x in line.split()[1:] if x.isdigit())
    except OSError:
        pass
    return -1


class Stop(Exception):
    pass


def main(argv):
    secs, watch, phase, sets, acks, sticky = 10.0, [], [], [], [], []
    max_hz, irq_limit, tag = 0, 3000, ""
    i = 0
    while i < len(argv):
        a, v = argv[i], argv[i + 1] if i + 1 < len(argv) else None
        if a == "--secs":
            secs = float(v)
        elif a == "--watch":
            watch += [x for x in v.split(",") if x]
        elif a == "--phase":
            phase += [x for x in v.split(",") if x]
        elif a == "--set":
            rf, val = v.split("=")
            reg, fld = rf.split(".")
            sets.append([reg, fld, int(val, 0), None])
        elif a == "--ack":
            rf, clr = v.split(":")
            reg, occ = rf.split(".")
            acks.append([reg, occ, clr, 0])
        elif a == "--clear-sticky":
            reg, fld = v.split(".")
            sticky.append((reg, fld))
        elif a == "--max-hz":
            max_hz = int(v)
        elif a == "--irq-limit":
            irq_limit = int(v)
        elif a == "--tag":
            tag = v
        else:
            sys.exit(__doc__)
        i += 2
    allow = {s[0] for s in sets} | {a[0] for a in acks} | {s[0] for s in sticky}
    for reg in list(allow) + watch + phase:
        dcnregs.off(reg)                               # unknown names fail before anything is written
    for s in sets:
        dcnregs.mask(s[0], s[1])
    for a in acks:
        dcnregs.mask(a[0], a[1]), dcnregs.mask(a[0], a[2])
    rd = dcnregs.Dbg(allow=allow)
    out = sys.stdout
    t0 = time.monotonic_ns()
    out.write("# dcnsample tag=%s %s secs=%s max_hz=%s irq_limit=%s\n" % (tag, dcnregs.stamp(), secs, max_hz,
                                                                          irq_limit))
    out.write("H t_ns n %s | %s\n" % (" ".join(watch), " ".join(phase)))

    def log_writes():
        while rd.wlog:
            t, n, old, new = rd.wlog.pop(0)
            out.write("W %d %s 0x%08X 0x%08X\n" % (t, n, old, new))

    def restore():
        for s in sets:
            if s[3] is not None:
                try:
                    rd.rmw(s[0], **{s[1]: s[3]})
                except OSError as e:
                    out.write("# restore %s.%s failed: %s\n" % (s[0], s[1], e))
        log_writes()

    def on_signal(signum, _frame):
        raise Stop("signal %d" % signum)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    rc, why = 0, "time"
    n = 0
    last = None
    lost = {}
    rate_n, rate_t = 0, t0
    irq0 = irq_last = irq_total()
    irq_t = t0
    irq_rate = 0
    try:
        for reg, fld in sticky:
            rd.rmw(reg, **{fld: 1})
        for s in sets:
            v = rd.r(s[0])
            s[3] = dcnregs.fget(s[0], s[1], v)
            rd.rmw(s[0], **{s[1]: s[2]})
        log_writes()
        period = 1e9 / max_hz if max_hz else 0
        end = t0 + int(secs * 1e9)
        nxt = t0
        guard = bool(sets or acks)
        while True:
            now = time.monotonic_ns()
            if now >= end:
                break
            if period:
                if now < nxt:
                    continue
                nxt += period
            w = [rd.r(r) for r in watch]
            p = [rd.r(r) for r in phase]
            t = time.monotonic_ns()
            n += 1
            if w != last:
                out.write("C %d %d %s | %s\n" % (t, n, " ".join("0x%08X" % x for x in w),
                                                 " ".join("0x%08X" % x for x in p)))
                last = w
            for a in acks:
                v = rd.r(a[0])
                if dcnregs.fget(a[0], a[1], v):
                    rd.w(a[0], dcnregs.fput(a[0], v, **{a[2]: 1}))
                    rd.wlog.clear()                      # acks are counted, not logged one by one
                    a[3] += 1
                    out.write("A %d %s.%s %d 0x%08X\n" % (t, a[0], a[1], a[3], v))
            if sets and n % 64 == 0:
                for s in sets:
                    v = rd.r(s[0])
                    if dcnregs.fget(s[0], s[1], v) != s[2]:
                        lost[s[0]] = lost.get(s[0], 0) + 1
                        rd.rmw(s[0], **{s[1]: s[2]})
                        log_writes()
            if t - irq_t >= 100_000_000:
                cur = irq_total()
                if irq_last >= 0 and cur >= 0:
                    irq_rate = (cur - irq_last) * 1e9 / (t - irq_t)
                irq_last, irq_t = cur, t
                if guard and irq_rate > irq_limit:
                    rc, why = 5, "irq storm %.0f/s > %d/s" % (irq_rate, irq_limit)
                    break
            if t - rate_t >= 1_000_000_000:
                out.write("S %d %d %.0f %.0f\n" % (t, n, (n - rate_n) * 1e9 / (t - rate_t), irq_rate))
                out.flush()
                rate_n, rate_t = n, t
    except Stop as e:
        why = str(e)
    finally:
        restore()
        t1 = time.monotonic_ns()
        irq1 = irq_total()
        out.write("E tag=%s why=%s samples=%d secs=%.3f rate_hz=%.0f irq_delta=%d acks=%s lost=%s rc=%d %s\n" % (
            tag, why, n, (t1 - t0) / 1e9, n * 1e9 / max(1, t1 - t0), irq1 - irq0,
            ",".join("%s.%s=%d" % (a[0], a[1], a[3]) for a in acks) or "-",
            ",".join("%s=%d" % kv for kv in lost.items()) or "-", rc, dcnregs.stamp()))
        out.flush()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
