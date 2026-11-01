#!/usr/bin/env python3
"""Display lane: generate the register tables of this directory with tools/regcalc (development PC only).

Every byte offset comes from regcalc over bc250-win/third_party/linux-amdgpu/dcn_2_0_1_offset.h (ip DMU)
or clk_11_0_1_offset.h (ip CLK). Nothing is typed by hand. Output lines: '<NAME> 0x<byte offset>'
(the format of scratch/linux-1031/regread.py). Left out on purpose: indexed data ports and index registers
(*_DATA*, *_INDEX*, LUT/RAM data), write-trigger registers (*MANUAL_TRIG*, *SOFT_RESET*, *_CLEAR registers
named so), pixel readback, test/debug ports, perfmon, audio endpoints, I2C/GPIO/AUX, HDMI/AFMT packet RAM.

Tables (tables/):
  gop-a.regs   the 75 registers E21 already read by raw BAR5 before amdgpu (gen_dmu_min.py set), first
  gop-b.regs   the firmware's pipe 0 path: HUBP0/HUBPREQ0/HUBPRET0, CNVC_CFG0, DPP_TOP0, DSCL0, CM0
               (control registers only), MPCC0, MPCC_OGAM0, MPC out, OPP0, FMT0, ODM0, OTG0, VTG0
  gop-c.regs   clocks and the output: DCCG, DENTIST, *CLK* control, OTG pixel rate, DIG0/1 FE/BE,
               DP0/1 link and stream, UNIPHYA/B, PHYPLL, SYMCLK, DIO power, DCHUBBUB, lock sets
  gop-d.regs   the other pipes and OTG1: HUBP1-3, HUBPREQ1-3, HUBPRET1-3, MPCC1-3, OTG1, OPP1, FMT1, ODM1
  gop-e.regs   CLK4 PLL request and the DPREFCLK counter (ip CLK), read last
  dcn-all.regs gop-a..e plus CNVC/DPP_TOP/DSCL/CM of pipes 1-3 (read only through amdgpu's debugfs, after
               the driver has powered them)
Usage: python gen-tables.py      (BC250_ROOT defaults to P:\\BC-250)
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(os.environ.get("BC250_ROOT", r"P:\BC-250"))
REPO = ROOT / "bc250-win"
HDR = REPO / "third_party" / "linux-amdgpu"
sys.path.insert(0, str(REPO / "tools" / "regcalc"))
from regcalc import RegMap  # noqa: E402

OUT = Path(__file__).resolve().parent / "tables"
dmu = RegMap(ip="DMU", reg_header=str(HDR / "dcn_2_0_1_offset.h"))
clk = RegMap(ip="CLK", reg_header=str(HDR / "clk_11_0_1_offset.h"))

EXCLUDE = re.compile(
    r"(_DATA$|3DLUT_DATA|_INDEX|_RAM_|COEF_RAM|MANUAL_TRIG|SOFT_RESET|READBACK|TEST_DEBUG|"
    r"PERFMON|_CRC|CRC_|_CLEAR$|AZ|I2C|GPIO|AUX|AFMT|HDMI_|TMDS_|DME_|GENERIC|SCRATCH|_SNAPSHOT_CONTROL|"
    r"COUNT_RESET|FORCE_COUNT_NOW|FORCE_NEXT_EYE|MANUAL_FORCE)")

names_all = sorted(n for n in dmu.regs)


def pick(prefixes, extra_exclude=None):
    out = []
    for n in names_all:
        s = n[2:]
        if not any(s.startswith(p) for p in prefixes):
            continue
        if EXCLUDE.search(s) or (extra_exclude and extra_exclude.search(s)):
            continue
        out.append(s)
    return out


# E21's set (experiments/E21-linux-reference-4/gen_dmu_min.py), same order, reproduced by name.
gop_a = []
for n in range(4):
    for r in ("DCSURF_PRIMARY_SURFACE_ADDRESS", "DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", "DCSURF_SURFACE_INUSE",
              "DCSURF_SURFACE_INUSE_HIGH", "DCSURF_SURFACE_CONTROL", "DCSURF_FLIP_CONTROL", "DCSURF_SURFACE_PITCH",
              "DCSURF_SURFACE_FLIP_INTERRUPT"):
        gop_a.append("HUBPREQ%d_%s" % (n, r))
    for r in ("DCHUBP_CNTL", "DCSURF_SURFACE_CONFIG", "DCSURF_ADDR_CONFIG", "DCSURF_TILING_CONFIG"):
        gop_a.append("HUBP%d_%s" % (n, r))
    gop_a.append("HUBPRET%d_HUBPRET_CONTROL" % n)
for m in range(2):
    for r in ("OTG_MASTER_UPDATE_LOCK", "OTG_CONTROL", "OTG_STATUS", "OTG_DOUBLE_BUFFER_CONTROL", "OTG_H_TOTAL",
              "OTG_V_TOTAL", "OTG_STATUS_POSITION", "OTG_VERTICAL_INTERRUPT0_CONTROL", "OTG_VERTICAL_INTERRUPT2_CONTROL",
              "OTG_GLOBAL_CONTROL0", "OTG_VUPDATE_PARAM"):
        gop_a.append("OTG%d_%s" % (m, r))
gop_a.append("DCHUBBUB_CTRL_STATUS")
gop_a = [s for s in gop_a if "mm" + s in dmu.regs]

CM_SKIP = re.compile(r"(RAMA_|RAMB_|_REGION_|LUT_WRITE_EN_MASK|3DLUT_OUT|SHAPER_LUT|BLNDGAM_LUT|DGAM_LUT)")
gop_b = pick(("HUBP0_", "HUBPREQ0_", "HUBPRET0_", "CNVC_CFG0_", "CNVC_CUR0_", "DPP_TOP0_", "DSCL0_", "MPCC0_",
              "MPCC_OGAM0_", "MPC_OUT0_", "OPP_PIPE0_", "OPPBUF0_", "FMT0_", "ODM0_", "OTG0_", "VTG0_",
              "MPC_CLOCK_CONTROL", "MPC_BYPASS_BG", "MPC_STALL", "MPC_OUT_CSC_COEF_FORMAT", "OPP_TOP_CLK_CONTROL",
              "OPTC_CLOCK_CONTROL"), None)
gop_b += [s for s in pick(("CM0_",)) if not CM_SKIP.search(s)]
gop_c = pick(("DCCG_", "DENTIST_", "DISPCLK_", "DPPCLK", "DPREFCLK_", "REFCLK_", "DCFCLK_", "SOCCLK_", "DSCCLK",
              "SYMCLK", "PHYPLL", "UNIPHY", "MICROSECOND_", "MILLISECOND_", "DIO_MEM_PWR", "DIO_CLK_CNTL",
              "DIO_POWER", "DCIO_CLOCK_CNTL", "DCE_VERSION", "DCHUBBUB_", "CFG_VUPDATE_LOCK", "ADR_",
              "OTG1_PIXEL_RATE_CNTL", "OTG1_PHYPLL_PIXEL_RATE_CNTL", "RBBMIF_STATUS", "DC_MEM_GLOBAL_PWR"),
             re.compile(r"(DCCG_AUDIO|DCHUBBUB_RET_PATH_DCC_CFG[0-7])"))
DIG_KEEP = re.compile(r"^DIG[01]_(DIG_FE_CNTL|DIG_BE_CNTL|DIG_BE_EN_CNTL|DIG_FIFO_STATUS|DIG_LANE_ENABLE|DIG_VERSION|"
                      r"DIG_CLOCK_PATTERN|DIG_TEST_PATTERN)$")
DP_KEEP = re.compile(r"^DP[01]_DP_(LINK_CNTL|PIXEL_FORMAT|MSA_COLORIMETRY|CONFIG|VID_STREAM_CNTL|STEER_FIFO|MSA_MISC|"
                     r"VID_TIMING|VID_N|VID_M|LINK_FRAMING_CNTL|VID_MSA_VBID|DPHY_CNTL|DPHY_TRAINING_PATTERN_SEL|"
                     r"DPHY_8B10B_CNTL|DPHY_SCRAM_CNTL|DPHY_FAST_TRAINING_STATUS|SEC_CNTL|MSE_RATE_CNTL|MSE_MISC_CNTL|"
                     r"MSA_TIMING_PARAM[1-4]|DSC_CNTL|DB_CNTL|MSA_VBID_MISC|DSC_BYTES_PER_PIXEL)$")
gop_c += [n[2:] for n in names_all if DIG_KEEP.match(n[2:]) or DP_KEEP.match(n[2:])]
gop_d = []
for i in (1, 2, 3):
    gop_d += pick(("HUBP%d_" % i, "HUBPREQ%d_" % i, "HUBPRET%d_" % i, "MPCC%d_" % i, "MPCC_OGAM%d_" % i))
gop_d += pick(("OTG1_", "OPP_PIPE1_", "OPPBUF1_", "FMT1_", "ODM1_", "VTG1_", "MPC_OUT1_", "MPCC4_"))
rest = []
for i in (1, 2, 3):
    rest += pick(("CNVC_CFG%d_" % i, "CNVC_CUR%d_" % i, "DPP_TOP%d_" % i, "DSCL%d_" % i))
    rest += [s for s in pick(("CM%d_" % i,)) if not CM_SKIP.search(s)]
clk_names = ["CLK4_0_CLK4_CLK_PLL_REQ", "CLK4_0_CLK4_CLK2_CURRENT_CNT"]


def write(name, regs, rm=dmu, seen=None):
    seen = set() if seen is None else seen
    lines = []
    for s in regs:
        if s in seen:
            continue
        seen.add(s)
        lines.append("%s 0x%05X" % (s, rm.byte_offset("mm" + s)))
    (OUT / name).write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    print("%-14s %4d registers" % (name, len(lines)))
    return lines


OUT.mkdir(exist_ok=True)
seen = set()
a = write("gop-a.regs", gop_a, seen=seen)
b = write("gop-b.regs", gop_b, seen=seen)
c = write("gop-c.regs", gop_c, seen=seen)
d = write("gop-d.regs", gop_d, seen=seen)
e = ["%s 0x%05X" % (n, clk.byte_offset("mm" + n)) for n in clk_names]
(OUT / "gop-e.regs").write_text("\n".join(e) + "\n", encoding="ascii", newline="\n")
print("gop-e.regs     %4d registers (ip CLK, clk_11_0_1_offset.h; read last, not read raw before)" % len(e))
r = write("rest.tmp", rest, seen=seen)
(OUT / "rest.tmp").unlink()
alln = a + b + c + d + e + r
(OUT / "dcn-all.regs").write_text("\n".join(alln) + "\n", encoding="ascii", newline="\n")
print("dcn-all.regs   %4d registers" % len(alln))
# Cross-check: every offset decodes back to its own name (regcalc reverse), no duplicates.
offs = {}
for line in alln:
    nm, off = line.split()
    off = int(off, 16)
    rmx = clk if nm.startswith("CLK4_") else dmu
    assert ("mm" + nm) in rmx.reverse(off), (nm, hex(off))
    assert off not in offs, (nm, offs.get(off))
    offs[off] = nm
print("reverse check: %d offsets, all unique, all decode to their names" % len(offs))
# Field masks of the same registers from dcn_2_0_1_sh_mask.h: lines '<REG> <FIELD> 0x<mask>' (the shift is the
# lowest set bit of the mask). The lab scripts decode and build values by field name from this file only.
mask_re = re.compile(r"^#define\s+(\w+?)__(\w+)_MASK\s+(0x[0-9A-Fa-f]+)L?\s*$")
fields = []
want = set(offs.values())
for line in (HDR / "dcn_2_0_1_sh_mask.h").read_text(encoding="ascii", errors="replace").splitlines():
    m = mask_re.match(line)
    if m and m.group(1) in want:
        fields.append("%s %s 0x%08X" % (m.group(1), m.group(2), int(m.group(3), 16)))
(OUT / "fields.txt").write_text("\n".join(fields) + "\n", encoding="ascii", newline="\n")
print("fields.txt     %4d fields of %d registers (dcn_2_0_1_sh_mask.h)" % (len(fields), len({f.split()[0] for f in fields})))
