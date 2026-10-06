#!/usr/bin/env python3
"""regcalc - byte addresses of amdgpu registers, computed from the kernel headers.

The only allowed source of register addresses in this project. Never type a BAR5
offset by hand; ask this tool (or the generated table in regs/).

Rule (amdgpu, soc15_common.h + amdgpu_reg_access.c):

    dword_index = IP_BASE[inst][mmREG_BASE_IDX] + mmREG
    byte_offset = dword_index * 4          # offset inside the register BAR (BAR5)

Usage:
    regcalc.py lookup mmGRBM_STATUS mmSCRATCH_REG0
    regcalc.py reverse 0x5C3C 0x9C1C       # what lives at this byte offset?
    regcalc.py grep WGP_MASK               # search register names
    regcalc.py table                       # key registers as a Markdown table
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HDR_DIR = REPO / "third_party" / "linux-amdgpu"
DEFAULT_IP_HEADER = HDR_DIR / "cyan_skillfish_ip_offset.h"
DEFAULT_REG_HEADER = HDR_DIR / "gc_10_1_0_offset.h"

# Registers every experiment starts from. Regenerate regs/gc-key-registers.md after changing it.
KEY_REGISTERS = [
    "mmGRBM_STATUS",
    "mmGRBM_STATUS2",
    "mmGRBM_SOFT_RESET",
    "mmGRBM_GFX_INDEX",
    "mmCC_GC_SHADER_ARRAY_CONFIG",
    "mmGC_USER_SHADER_ARRAY_CONFIG",
    "mmSPI_PG_ENABLE_STATIC_WGP_MASK",
    "mmRLC_PG_ALWAYS_ON_WGP_MASK",
    "mmSCRATCH_REG0",
    "mmCP_ME_CNTL",
    "mmCP_MEC_CNTL",
    "mmCP_RB0_BASE",
    "mmCP_RB0_BASE_HI",
    "mmCP_RB0_CNTL",
    "mmCP_RB0_RPTR",
    "mmCP_RB0_WPTR",
    "mmCP_HQD_PQ_BASE",
    "mmCP_HQD_PQ_CONTROL",
    "mmRLC_CNTL",
    "mmRLC_CP_SCHEDULERS",
]

_SEG_RE = re.compile(r"^#define\s+(\w+?)_BASE__INST(\d)_SEG(\d)\s+(0x[0-9A-Fa-f]+|\d+)\s*$")
_REG_RE = re.compile(r"^#define\s+(mm\w+)\s+(0x[0-9A-Fa-f]+)\s*$")
_IDX_RE = re.compile(r"^#define\s+(mm\w+)_BASE_IDX\s+(\d+)\s*$")
# Indirect indices: AMD's ix<NAME> defines, an index into a block's own INDEX/DATA register pair (for example the
# Azalia endpoint's AZF0ENDPOINTn_AZALIA_F0_CODEC_ENDPOINT_INDEX/_DATA in dcn_2_0_1_offset.h). They are not BAR5
# offsets and have no segment: the value is what the driver writes into the INDEX register.
_IX_RE = re.compile(r"^#define\s+(ix\w+)\s+(0x[0-9A-Fa-f]+)\s*$")


def parse_ip_bases(path):
    """{ip: {inst: {seg: dword_base}}} from *_ip_offset.h"""
    bases = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = _SEG_RE.match(line)
        if m:
            ip, inst, seg, val = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4), 0)
            bases.setdefault(ip, {}).setdefault(inst, {})[seg] = val
    return bases


def parse_registers(path):
    """{name: (mm_offset, base_idx)} from *_offset.h"""
    offs, idxs = {}, {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = _IDX_RE.match(line)
        if m:
            idxs[m.group(1)] = int(m.group(2))
            continue
        m = _REG_RE.match(line)
        if m:
            offs[m.group(1)] = int(m.group(2), 16)
    return {n: (o, idxs[n]) for n, o in offs.items() if n in idxs}


def parse_indices(path):
    """{ixNAME: index} from *_offset.h: the indirect register indices the header defines (may be empty)"""
    found = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = _IX_RE.match(line)
        if m:
            found[m.group(1)] = int(m.group(2), 16)
    return found


class RegMap:
    def __init__(self, ip="GC", inst=0, ip_header=DEFAULT_IP_HEADER, reg_header=DEFAULT_REG_HEADER):
        self.ip, self.inst = ip, inst
        self.segs = parse_ip_bases(ip_header)[ip][inst]
        self.regs = parse_registers(reg_header)
        self.ix = parse_indices(reg_header)

    def index(self, name):
        """The indirect index of an ix name: what goes into the block's INDEX register, never a BAR5 offset."""
        return self.ix[name]

    def byte_offset(self, name):
        mm, idx = self.regs[name]
        return (self.segs[idx] + mm) * 4

    def reverse(self, byte_off):
        """Register names of this IP that decode to byte_off (may be empty)."""
        if byte_off % 4:
            return []
        dword = byte_off // 4
        return sorted(n for n, (mm, idx) in self.regs.items() if self.segs.get(idx, 0) + mm == dword)


def _norm(name):
    return name if name.startswith(("mm", "ix")) else "mm" + name


def cmd_lookup(rm, names):
    rc = 0
    for raw in names:
        name = _norm(raw)
        if name.startswith("ix"):
            if name not in rm.ix:
                print(f"{name}: NOT FOUND in header (no such indirect index for {rm.ip})")
                rc = 1
                continue
            print(f"{name}: indirect index 0x{rm.ix[name]:04X} (written to the block's INDEX register, not a BAR5 offset)")
            continue
        if name not in rm.regs:
            print(f"{name}: NOT FOUND in header (this register does not exist for {rm.ip})")
            rc = 1
            continue
        mm, idx = rm.regs[name]
        print(f"{name}: mm=0x{mm:04X} seg{idx}=0x{rm.segs[idx]:X} -> BAR5+0x{rm.byte_offset(name):05X}")
    return rc


def cmd_reverse(rm, addrs):
    for raw in addrs:
        off = int(raw, 0)
        names = rm.reverse(off)
        where = ", ".join(names) if names else f"no {rm.ip} register at this offset"
        print(f"BAR5+0x{off:05X} (dword 0x{off // 4:05X}): {where}")
    return 0


def cmd_grep(rm, pattern):
    rx = re.compile(pattern, re.IGNORECASE)
    for name in sorted(n for n in rm.regs if rx.search(n)):
        print(f"{name}: BAR5+0x{rm.byte_offset(name):05X}")
    for name in sorted(n for n in rm.ix if rx.search(n)):
        print(f"{name}: indirect index 0x{rm.ix[name]:04X}")
    return 0


def cmd_table(rm):
    print("| Register | mm | seg | BAR5 byte offset |")
    print("|---|---|---|---|")
    for name in KEY_REGISTERS:
        mm, idx = rm.regs[name]
        print(f"| `{name}` | 0x{mm:04X} | {idx} | `0x{rm.byte_offset(name):05X}` |")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default="GC")
    ap.add_argument("--inst", type=int, default=0)
    ap.add_argument("--ip-header", default=DEFAULT_IP_HEADER)
    ap.add_argument("--reg-header", default=DEFAULT_REG_HEADER)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("lookup").add_argument("names", nargs="+")
    sub.add_parser("reverse").add_argument("addrs", nargs="+")
    sub.add_parser("grep").add_argument("pattern")
    sub.add_parser("table")
    args = ap.parse_args(argv)

    rm = RegMap(args.ip, args.inst, args.ip_header, args.reg_header)
    if args.cmd == "lookup":
        return cmd_lookup(rm, args.names)
    if args.cmd == "reverse":
        return cmd_reverse(rm, args.addrs)
    if args.cmd == "grep":
        return cmd_grep(rm, args.pattern)
    return cmd_table(rm)


if __name__ == "__main__":
    sys.exit(main())
