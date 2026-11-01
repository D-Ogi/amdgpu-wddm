#!/usr/bin/env python3
"""Display lane D3 (L28): check whether FLIP_INTERRUPT and VSTARTUP vectors fire on this part.

R1: writes to HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT and OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL,
restored on exit. The dcnregs.Dbg allow list limits writes to exactly these two registers.

1. Record /proc/interrupts baseline.
2. Enable DCSURF_PRIMARY_SURFACE_FLIP_INT_EN in HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT.
3. Enable OTG_VERTICAL_INTERRUPT0_INT_EN in OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL.
4. Run modetest (XR24, 3 s hold) to trigger flips and vblanks.
5. Record /proc/interrupts again.
6. Restore both registers.
7. Report the delta.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcnregs

FLIP_REG = "HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT"
FLIP_FIELD = "SURFACE_FLIP_INT_MASK"
VSTART_REG = "OTG0_OTG_VERTICAL_INTERRUPT0_CONTROL"
VSTART_FIELD = "OTG_VERTICAL_INTERRUPT0_INT_ENABLE"

OUT = "/root/disp/l28-irq.txt"

IRQ_BAIL = 10000


def amdgpu_irqs():
    total = 0
    for line in open("/proc/interrupts"):
        if "amdgpu" in line:
            total += sum(int(x) for x in line.split()[1:] if x.isdigit())
    return total


def main():
    f = open(OUT, "w")

    def log(s):
        f.write(s + "\n")
        f.flush()
        print(s)

    log("# d3-l28-irq %s" % dcnregs.stamp())

    dcnregs.off(FLIP_REG)
    dcnregs.off(VSTART_REG)
    dcnregs.mask(FLIP_REG, FLIP_FIELD)
    dcnregs.mask(VSTART_REG, VSTART_FIELD)

    rd = dcnregs.Dbg(allow={FLIP_REG, VSTART_REG})
    old_flip = rd.r(FLIP_REG)
    old_vstart = rd.r(VSTART_REG)
    log("baseline FLIP_REG=0x%08X %s" % (old_flip, dcnregs.decode(FLIP_REG, old_flip, all_fields=True)))
    log("baseline VSTART_REG=0x%08X %s" % (old_vstart, dcnregs.decode(VSTART_REG, old_vstart, all_fields=True)))

    irq0 = amdgpu_irqs()
    log("irq_baseline=%d" % irq0)

    try:
        # Enable the two vectors
        rd.rmw(FLIP_REG, **{FLIP_FIELD: 1})
        rd.rmw(VSTART_REG, **{VSTART_FIELD: 1})
        for t, n, old, new in rd.wlog:
            log("W %d %s 0x%08X -> 0x%08X" % (t, n, old, new))
        rd.wlog.clear()

        en_flip = rd.r(FLIP_REG)
        en_vstart = rd.r(VSTART_REG)
        log("enabled FLIP_REG=0x%08X %s" % (en_flip, dcnregs.decode(FLIP_REG, en_flip, all_fields=True)))
        log("enabled VSTART_REG=0x%08X %s" % (en_vstart, dcnregs.decode(VSTART_REG, en_vstart, all_fields=True)))

        irq1 = amdgpu_irqs()
        log("irq_after_enable=%d delta=%d" % (irq1, irq1 - irq0))
        if irq1 - irq0 > IRQ_BAIL:
            log("BAIL: irq storm after enable (%d new), restoring" % (irq1 - irq0))
            raise SystemExit(5)

        # Trigger flips with modetest (holds the DRM master for about 3 s)
        log("# modetest start %s" % dcnregs.stamp())
        p = subprocess.Popen(
            ["modetest", "-M", "amdgpu", "-s", "64@58:1920x1200", "-P", "45@58:1920x1200@XR24"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        mt_out = p.stdout.read().decode(errors="replace").strip()
        log("modetest rc=%d out=%s" % (p.returncode, mt_out[:200]))

        # Check for irq storm during the flip
        irq_mid = amdgpu_irqs()
        log("irq_during_modetest=%d delta_from_enable=%d" % (irq_mid, irq_mid - irq1))
        if irq_mid - irq1 > IRQ_BAIL:
            log("BAIL: irq storm during modetest (%d new)" % (irq_mid - irq1))
            raise SystemExit(5)

        # Wait a moment for any deferred delivery
        time.sleep(1)
        irq2 = amdgpu_irqs()
        log("irq_final=%d delta_total=%d" % (irq2, irq2 - irq0))

    finally:
        # Restore the original register values
        rd.w(FLIP_REG, old_flip)
        rd.w(VSTART_REG, old_vstart)
        for t, n, old, new in rd.wlog:
            log("W %d %s 0x%08X -> 0x%08X" % (t, n, old, new))
        rd.wlog.clear()
        check_flip = rd.r(FLIP_REG)
        check_vstart = rd.r(VSTART_REG)
        log("restored FLIP_REG=0x%08X (was 0x%08X)" % (check_flip, old_flip))
        log("restored VSTART_REG=0x%08X (was 0x%08X)" % (check_vstart, old_vstart))

    delta = irq2 - irq0
    if delta > 0:
        log("RESULT: %d new amdgpu interrupts; the enabled vectors fire on this part" % delta)
    else:
        log("RESULT: 0 new amdgpu interrupts; the vectors do not fire when enabled")

    log("# d3 done %s" % dcnregs.stamp())
    f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
