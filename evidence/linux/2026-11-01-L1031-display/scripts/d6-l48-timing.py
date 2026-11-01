#!/usr/bin/env python3
"""Display lane D6 (L48-DCN201): two-plane update timing.

Risk R0: read only. Samples the pending/lock/frame-counter state while two planes flip.

1. Run modetest in the background: primary + overlay.
2. Start dcnsample watching the pending bits, lock and MPCC status.
3. Run three rapid modetest flips on the primary to produce pending-bit transitions.
4. Report the acceptance window relative to VUPDATE.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcnregs
import dcnsample

OUT = "/root/disp/l48-timing.txt"


def main():
    f = open(OUT, "w")

    def log(s):
        f.write(s + "\n")
        f.flush()
        print(s)

    log("# d6-l48-timing %s" % dcnregs.stamp())

    # First, set up two-plane mode and identify which pipes carry them.
    log("# setting up primary + overlay")
    p = subprocess.Popen(
        ["modetest", "-M", "amdgpu",
         "-s", "64@58:1920x1200",
         "-P", "49@58:320x200+100+100@AR24"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    time.sleep(2)

    if p.poll() is not None:
        mt_out = p.stdout.read().decode(errors="replace").strip()
        log("modetest exited early rc=%d: %s" % (p.returncode, mt_out[:500]))
        f.close()
        return p.returncode if p.returncode else 1

    # Find the overlay pipe: check which HUBP instances are live besides the primary's pipes
    rd = dcnregs.Dbg()
    live_pipes = []
    for n in range(4):
        try:
            clk = rd.r("HUBP%d_HUBP_CLK_CNTL" % n)
            cntl = rd.r("HUBP%d_DCHUBP_CNTL" % n)
            opp = rd.r("MPCC%d_MPCC_OPP_ID" % n)
            on = dcnregs.fget("HUBP%d_HUBP_CLK_CNTL" % n, "HUBP_CLOCK_ENABLE", clk)
            blank = dcnregs.fget("HUBP%d_DCHUBP_CNTL" % n, "HUBP_BLANK_EN", cntl)
            oppid = dcnregs.fget("MPCC%d_MPCC_OPP_ID" % n, "MPCC_OPP_ID", opp)
            if on and not blank and oppid != 0xF:
                dim = rd.r("HUBP%d_DCSURF_PRI_VIEWPORT_DIMENSION" % n)
                w = dim & 0x3FFF
                h = (dim >> 16) & 0x3FFF
                live_pipes.append((n, oppid, w, h))
                log("live pipe %d: opp%d %dx%d" % (n, oppid, w, h))
        except (KeyError, OSError) as e:
            log("pipe %d: ERR %s" % (n, e))

    if len(live_pipes) < 2:
        log("ONLY %d live pipes (need 2 for two-plane timing); aborting" % len(live_pipes))
        p.kill()
        p.wait()
        f.close()
        return 1

    # Build watch and phase register lists based on live pipes
    watch = []
    for n, _, _, _ in live_pipes:
        watch.append("HUBPREQ%d_DCSURF_SURFACE_CONTROL" % n)
    watch.append("OTG0_OTG_MASTER_UPDATE_LOCK")
    watch.append("MPCC0_MPCC_STATUS")

    phase = ["OTG0_OTG_STATUS_POSITION", "OTG0_OTG_STATUS_FRAME_COUNT"]

    log("# watch: %s" % ",".join(watch))
    log("# phase: %s" % ",".join(phase))
    log("")

    # Sample for 15 s at high rate
    log("# starting sampler %s" % dcnregs.stamp())
    rd.close()
    argv = [
        "--secs", "15",
        "--max-hz", "5000",
        "--watch", ",".join(watch),
        "--phase", ",".join(phase),
        "--tag", "l48-timing",
    ]
    # Run dcnsample in a child process so its output goes to our file
    sp = subprocess.Popen(
        ["python3", "-u", "dcnsample.py"] + argv,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    time.sleep(2)

    # Trigger flips: three rapid mode sets on the primary to produce pending-bit transitions.
    # Each modetest -s exits after programming; the overlay's modetest still holds the master,
    # so we flip the primary by doing vsynced flips with -v.
    log("# starting vsynced primary flips %s" % dcnregs.stamp())
    pf = subprocess.Popen(
        ["timeout", "10", "modetest", "-M", "amdgpu",
         "-s", "64@58:1920x1200", "-P", "45@58:1920x1200@XR24", "-v"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    pf.wait()
    pf_out = pf.stdout.read().decode(errors="replace")
    for line in pf_out.strip().splitlines()[-5:]:
        log("flip: %s" % line)

    # Wait for the sampler
    try:
        sp.wait(timeout=20)
    except subprocess.TimeoutExpired:
        sp.kill()
        sp.wait()
    samp_out = sp.stdout.read().decode(errors="replace")
    f.write(samp_out)

    # Count changes and S lines from the sampler output
    changes = sum(1 for line in samp_out.splitlines() if line.startswith("C "))
    stats = [line for line in samp_out.splitlines() if line.startswith("S ")]
    log("")
    log("# sampler: %d change lines, %d stat lines" % (changes, len(stats)))

    log("# killing overlay modetest %s" % dcnregs.stamp())
    p.kill()
    p.wait()

    log("# d6 done %s" % dcnregs.stamp())
    f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
