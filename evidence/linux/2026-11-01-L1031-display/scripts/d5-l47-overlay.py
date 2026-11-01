#!/usr/bin/env python3
"""Display lane D5 (L47-DCN201): two-plane RGB alpha overlay and register capture.

Risk R0: read only. modetest sets up the two-plane configuration (normal driver operation);
this script reads the DCN registers while it is live.

1. Run modetest in the background: primary plane 45 + overlay plane 49 on CRTC 58.
2. While modetest holds the DRM master, read the HUBP/CNVC/MPCC/CM registers of every
   pipe that carries a plane (dcnregs.pipes to find them, then a targeted dump).
3. modetest exits and tears down the overlay.
4. Record the MPCC tree and the alpha/blend configuration.

No YUV: no plane on this kernel advertises NV12, P010 or AYUV.
No CSC bank-status: DCN201 headers have no such field.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcnregs

OUT = "/root/disp/l47-overlay.txt"

# Registers to read for each pipe N
PIPE_DUMP = [
    "HUBP{n}_DCHUBP_CNTL",
    "HUBP{n}_DCSURF_SURFACE_CONFIG",
    "HUBP{n}_DCSURF_PRI_VIEWPORT_START",
    "HUBP{n}_DCSURF_PRI_VIEWPORT_DIMENSION",
    "HUBP{n}_HUBP_CLK_CNTL",
    "HUBP{n}_DCHUBP_REQ_SIZE_CONFIG",
    "HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS",
    "HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH",
    "HUBPREQ{n}_DCSURF_SURFACE_PITCH",
    "HUBPREQ{n}_DCSURF_SURFACE_CONTROL",
    "HUBPREQ{n}_DCSURF_FLIP_CONTROL",
    "HUBPREQ{n}_DCN_EXPANSION_MODE",
    "HUBPREQ{n}_DCN_GLOBAL_TTU_CNTL",
    "HUBPREQ{n}_DCN_SURF0_TTU_CNTL0",
    "HUBPREQ{n}_DCN_SURF0_TTU_CNTL1",
    "HUBPRET{n}_HUBPRET_CONTROL",
    "CNVC_CFG{n}_CNVC_SURFACE_PIXEL_FORMAT",
    "CNVC_CFG{n}_FORMAT_CONTROL",
    "CNVC_CFG{n}_ALPHA_2BIT_LUT",
    "DPP_TOP{n}_DPP_CONTROL",
]

# MPCC/MPC registers for all 5 instances (0-4)
MPCC_DUMP = [
    "MPCC{n}_MPCC_TOP_SEL",
    "MPCC{n}_MPCC_BOT_SEL",
    "MPCC{n}_MPCC_OPP_ID",
    "MPCC{n}_MPCC_CONTROL",
    "MPCC{n}_MPCC_STATUS",
    "MPCC{n}_MPCC_SM_CONTROL",
    "MPCC{n}_MPCC_UPDATE_LOCK_SEL",
]

GLOBAL = [
    "MPC_OUT0_CSC_MODE",
    "MPC_OUT0_MUX",
    "MPC_OUT0_DENORM_CONTROL",
    "MPC_OUT_CSC_COEF_FORMAT",
    "MPC_BYPASS_BG_AR",
    "MPC_BYPASS_BG_GB",
    "MPC_CLOCK_CONTROL",
    "ODM0_OPTC_DATA_SOURCE_SELECT",
    "ODM0_OPTC_INPUT_GLOBAL_CONTROL",
    "OTG0_OTG_CONTROL",
    "OTG0_OTG_MASTER_UPDATE_LOCK",
    "OTG0_OTG_STATUS",
    "OTG0_OTG_STATUS_FRAME_COUNT",
    "OPPBUF0_OPPBUF_CONTROL",
]


def main():
    f = open(OUT, "w")

    def log(s):
        f.write(s + "\n")
        f.flush()
        print(s)

    log("# d5-l47-overlay %s" % dcnregs.stamp())

    # Start modetest: primary + overlay.
    # -s 64@58:1920x1200 sets the mode on connector 64, CRTC 58.
    # -P 49@58:320x200+100+100@AR24 sets overlay plane 49 on the same CRTC, with alpha.
    log("# starting modetest with primary + overlay")
    p = subprocess.Popen(
        ["modetest", "-M", "amdgpu",
         "-s", "64@58:1920x1200",
         "-P", "49@58:320x200+100+100@AR24"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # Give the mode set time to take effect
    time.sleep(2)

    if p.poll() is not None:
        mt_out = p.stdout.read().decode(errors="replace").strip()
        log("modetest exited early rc=%d: %s" % (p.returncode, mt_out[:500]))
        f.close()
        return p.returncode if p.returncode else 1

    rd = dcnregs.Dbg()
    log("# %s %s" % (rd.path, dcnregs.stamp()))

    # Find live pipes (pipes() prints to stdout and returns the live list)
    log("")
    log("# pipes check")
    # Capture pipes output by redirecting stdout temporarily
    old_stdout = sys.stdout
    sys.stdout = f
    live = dcnregs.pipes(rd)
    sys.stdout = old_stdout

    # Dump registers for every pipe instance 0-3
    log("")
    log("# per-pipe registers")
    for n in range(4):
        log("# pipe %d" % n)
        for tmpl in PIPE_DUMP:
            r = tmpl.format(n=n)
            try:
                v = rd.r(r)
                log("%-56s 0x%08X  %s" % (r, v, dcnregs.decode(r, v)))
            except (KeyError, OSError) as e:
                log("%-56s ERR %s" % (r, e))

    # MPCC tree (instances 0-4)
    log("")
    log("# MPCC tree")
    for n in range(5):
        for tmpl in MPCC_DUMP:
            r = tmpl.format(n=n)
            try:
                v = rd.r(r)
                log("%-56s 0x%08X  %s" % (r, v, dcnregs.decode(r, v)))
            except (KeyError, OSError) as e:
                log("%-56s ERR %s" % (r, e))

    # Global/OTG registers
    log("")
    log("# global / OTG")
    for r in GLOBAL:
        try:
            v = rd.r(r)
            log("%-56s 0x%08X  %s" % (r, v, dcnregs.decode(r, v)))
        except (KeyError, OSError) as e:
            log("%-56s ERR %s" % (r, e))

    # CM CSC mode for pipes 0-3 (no bank-status on DCN201)
    log("")
    log("# CM CSC mode (no bank-status field on DCN201)")
    for n in range(4):
        for r in ("CM%d_CM_ICSC_CONTROL" % n, "CM%d_CM_ICSC_C11_C12" % n):
            try:
                v = rd.r(r)
                log("%-56s 0x%08X  %s" % (r, v, dcnregs.decode(r, v)))
            except (KeyError, OSError) as e:
                log("%-56s ERR %s" % (r, e))

    log("")
    log("# d5 done, killing modetest %s" % dcnregs.stamp())
    p.kill()
    p.wait()
    mt_out = p.stdout.read().decode(errors="replace").strip()
    log("modetest rc=%d out=%s" % (p.returncode, mt_out[:200]))

    f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
