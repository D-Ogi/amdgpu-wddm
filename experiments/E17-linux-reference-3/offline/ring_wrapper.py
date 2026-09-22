#!/usr/bin/env python3
"""Decode what amdgpu wraps around a user IB, from the ring dumps we already have.

    python experiments/E17-linux-reference-3/offline/ring_wrapper.py <ring dump ...>
    python experiments/E17-linux-reference-3/offline/ring_wrapper.py --selftest

The ring dumps in evidence/ were taken with `dd ... | od -A x -t x4 -v` and were only ever read
for their INDIRECT_BUFFER packets (`decode_cs.py --ring`, which is deliberately conservative:
"only the packet kind we are sure of is reported"). Everything else the CP executed - the
pipeline sync, the page table base writes, the TLB invalidation, the pasid mapping, the fences -
was captured in the same bytes and never decoded. That is what this does, offline, on evidence
that is not modified.

Nothing here is typed by hand, per rule 1 of bc250-win/CLAUDE.md:

  packet names      E14's decode_cs.py, which parses `#define PACKET3_<NAME>` out of a reference
                    header. The default here is the kernel's nvd.h rather than E14's Mesa sid.h,
                    because a ring is emitted by the kernel and uses packets Mesa never writes:
                    sid.h has no name for opcode 0x8B, nvd.h calls it SWITCH_BUFFER. Pass
                    --pkt-header to use Mesa's instead; where both name an opcode, they agree.
  register names    tools/regcalc, from the AMD headers. WRITE_DATA and WAIT_REG_MEM carry an
                    absolute dword address, so the BAR5 byte offset is that address << 2 - the
                    same arithmetic decode_cs.py does for the SET_*_REG windows.
  field layouts     the PM4 definitions in ref/linux-src/.../nvd.h and Mesa's sid.h; each is
                    named in a comment next to the code that reads it.

`--selftest` is the positive control: a synthetic ring built here must come back with the
register names and the VMID the arithmetic says, before any real dump is believed.
"""

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "experiments" / "E14-vulkan-compute-reference"))
sys.path.insert(0, str(REPO / "tools" / "regcalc"))
import decode_cs                                    # noqa: E402
import regcalc                                      # noqa: E402

# WRITE_DATA, PM4 spec / nvd.h: control dword, DST_SEL bits 11:8, WR_CONFIRM bit 20,
# ENGINE_SEL bits 31:30. DST_SEL 0 is "mem-mapped register", 5 is "memory, confirm".
WD_DST_SEL = {0: "register", 1: "memory-sync", 2: "tc-l2", 3: "gds", 5: "memory", 6: "memory-nowait"}
WD_ENGINE = {0: "ME", 1: "PFP", 2: "CE", 3: "DE"}

# WAIT_REG_MEM, dword 1: FUNCTION bits 2:0, MEM_SPACE bit 4, OPERATION bits 7:6, ENGINE bit 8.
WRM_FUNC = {0: "always", 1: "<", 2: "<=", 3: "==", 4: "!=", 5: ">=", 6: ">"}
WRM_OP = {0: "wait_reg_mem", 1: "wr_wait_wr_reg", 2: "wait_mem_preemptable"}

# RELEASE_MEM, dword 2: DST_SEL bits 17:16, INT_SEL bits 26:24, DATA_SEL bits 31:29.
RM_DATA_SEL = {0: "none", 1: "32-bit data", 2: "64-bit data", 3: "gpu clock", 5: "timestamp"}


class Wrapper:
    def __init__(self, header=None):
        self.ops, self.windows = decode_cs.load_header(header or decode_cs.KERNEL_NVD)
        self.regmap = regcalc.RegMap()
        # The ring also writes registers outside GC. The pasid-to-VMID map the VM flush programs is
        # mmIH_VMID_<vmid>_LUT, which lives in OSSSYS (amdgpu_amdkfd_gfx_v10.c:130 writes
        # SOC15_REG_OFFSET(OSSSYS, 0, mmIH_VMID_0_LUT) + vmid), so that header is consulted too.
        # Without it the LUT write decoded as "<unnamed BAR5+0x04284>".
        self.regmaps = [self.regmap]
        for ip, header in (("OSSSYS", "osssys_5_0_0_offset.h"),):
            path = REPO / "third_party" / "linux-amdgpu" / header
            if path.exists():
                self.regmaps.append(regcalc.RegMap(ip=ip, reg_header=path))
        self.dec = decode_cs.Decoder(self.ops, self.windows, self.regmap)
        self.by_name = {name: op for op, name in self.ops.items()}

    def reg(self, dword_addr):
        """A register named by regcalc from an absolute dword address. Never a typed offset."""
        byte = dword_addr << 2
        for regmap in self.regmaps:
            names = regmap.reverse(byte)
            if names:
                return ("/".join(names), byte)
        return (f"<unnamed BAR5+0x{byte:05X}>", byte)

    # ---------------------------------------------------------------- per-packet decode
    def body(self, name, body, out):
        if name == "WRITE_DATA" and len(body) >= 4:
            ctl = body[0]
            dst = ctl >> 8 & 0xF
            engine = ctl >> 30 & 3
            where = WD_DST_SEL.get(dst, f"dst_sel={dst}")
            if dst == 0:
                rname, byte = self.reg(body[1])
                for k, value in enumerate(body[3:]):
                    more, mbyte = self.reg(body[1] + k)
                    out.append(f"          {more}  (BAR5+0x{mbyte:05X}) <- 0x{value:08x}"
                               f"   [{WD_ENGINE.get(engine, engine)}]")
                del rname, byte
            else:
                addr = (body[2] << 32) | body[1]
                out.append(f"          {where} at 0x{addr:012x} <- "
                           + " ".join(f"{v:08x}" for v in body[3:]))

        elif name == "WAIT_REG_MEM" and len(body) >= 6:
            f = body[0]
            func, space, op = f & 7, f >> 4 & 1, f >> 6 & 3
            ref, mask, interval = body[3], body[4], body[5]
            if op == 1:
                # WR_WAIT_WR_REG: write `ref` to the register in dword 1, then poll the register
                # in dword 2 until (value & mask) == mask. This is how a TLB invalidation is
                # issued from the ring (gmc_v10_0.c:396-408).
                wreg, wbyte = self.reg(body[1])
                preg, pbyte = self.reg(body[2])
                vmid = mask.bit_length() - 1 if mask and not (mask & (mask - 1)) else None
                out.append(f"          write 0x{ref:08x} -> {wreg}  (BAR5+0x{wbyte:05X})")
                out.append(f"          poll  {preg}  (BAR5+0x{pbyte:05X}) until & 0x{mask:x}"
                           f"{f'  (bit {vmid} = vmid {vmid})' if vmid is not None else ''}"
                           f", every {interval} clocks")
            elif space:
                addr = ((body[2] << 32) | body[1]) & ~3
                out.append(f"          poll memory 0x{addr:012x} {WRM_FUNC.get(func, func)} "
                           f"0x{ref:08x} mask 0x{mask:08x}, every {interval} clocks")
            else:
                rname, byte = self.reg(body[1])
                out.append(f"          poll {rname}  (BAR5+0x{byte:05X}) "
                           f"{WRM_FUNC.get(func, func)} 0x{ref:08x} mask 0x{mask:08x}")

        elif name == "INDIRECT_BUFFER" and len(body) >= 3:
            va = (body[1] << 32) | (body[0] & 0xFFFFFFFC)
            ctl = body[2]
            out.append(f"          IB at VA 0x{va:016x}  {ctl & 0xFFFFF} dwords  "
                       f"VMID={ctl >> 24 & 0xF}  CHAIN={ctl >> 20 & 1}  "
                       f"PRE_ENA={ctl >> 21 & 1}  VALID={ctl >> 23 & 1}")

        elif name == "RELEASE_MEM" and len(body) >= 6:
            sel = body[1]
            addr = ((body[3] << 32) | body[2]) & ~3
            data = (body[5] << 32) | body[4]
            out.append(f"          event 0x{body[0] & 0x3F:02x}, "
                       f"{RM_DATA_SEL.get(sel >> 29 & 7, sel >> 29 & 7)} 0x{data:x} "
                       f"-> 0x{addr:012x}")

        elif name == "CONTEXT_CONTROL" and len(body) >= 2:
            out.append(f"          load_cntl 0x{body[0]:08x}  shadow_cntl 0x{body[1]:08x}")

    # ---------------------------------------------------------------- the walk
    def walk(self, words, out, skip_nops=True):
        counts = {}
        i, n = 0, len(words)
        nop_run = 0
        while i < n:
            hdr = words[i]
            if hdr >> 30 != 3:
                i += 1
                continue
            op = hdr >> 8 & 0xFF
            total = (hdr >> 16 & 0x3FFF) + 2
            if i + total > n:
                break
            name = self.ops.get(op, f"UNKNOWN_OP_0x{op:02X}")
            counts[name] = counts.get(name, 0) + 1
            if name == "NOP" and skip_nops:
                nop_run += total
                i += total
                continue
            if nop_run:
                out.append(f"  ---- {nop_run} dwords of NOP padding ----")
                nop_run = 0
            out.append(f"  [dw {i:5d}] {hdr:08x}  {name}  ({total} dwords)")
            self.body(name, words[i + 1:i + total], out)
            i += total
        return counts


def selftest(w):
    """Build a ring by hand and require the decode to come back with the right names."""
    ok = True
    base_lo = w.regmap.byte_offset("mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32")
    req = w.regmap.byte_offset("mmGCVM_INVALIDATE_ENG4_REQ")
    ack = w.regmap.byte_offset("mmGCVM_INVALIDATE_ENG4_ACK")
    for name, hit in (("mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32", base_lo),
                      ("mmGCVM_INVALIDATE_ENG4_REQ", req), ("mmGCVM_INVALIDATE_ENG4_ACK", ack)):
        print(f"  regcalc {name:48s} "
              + (f"BAR5+0x{hit:05X}" if hit is not None else "NOT FOUND"))
        ok &= hit is not None
    if not ok:
        print("\nSELFTEST FAILED: regcalc does not know these registers")
        return 1

    wd, wrm, ib = w.by_name["WRITE_DATA"], w.by_name["WAIT_REG_MEM"], w.by_name["INDIRECT_BUFFER"]
    words = [
        (3 << 30) | (wd << 8) | (3 << 16), 0x00100000, base_lo >> 2, 0, 0x6FFE0001,
        (3 << 30) | (wrm << 8) | (5 << 16), 0x43, req >> 2, ack >> 2,
        0x00F80002, 0x00000002, 0x00000020,
        (3 << 30) | (ib << 8) | (2 << 16), 0x00100000, 0x00000001, (7 << 24) | 0x48,
    ]
    out = []
    w.walk(words, out)
    text = "\n".join(out)
    print()
    print(text)
    checks = [("mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32", "the base register is named"),
              ("mmGCVM_INVALIDATE_ENG4_REQ", "the invalidation request is named"),
              ("mmGCVM_INVALIDATE_ENG4_ACK", "the acknowledgement is named"),
              ("bit 1 = vmid 1", "the ack mask is read as a vmid"),
              ("VMID=7", "the IB's vmid is read")]
    print()
    for needle, what in checks:
        hit = needle in text
        ok &= hit
        print(f"  {'OK      ' if hit else 'MISMATCH'}  {what}")
    print("\nSELFTEST PASSED" if ok else "\nSELFTEST FAILED")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumps", nargs="*", help="ring dumps from `dd | od -A x -t x4 -v`")
    ap.add_argument("--pkt-header", default=None)
    ap.add_argument("--nops", action="store_true", help="print the NOP padding packet by packet")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    w = Wrapper(args.pkt_header)
    if args.selftest:
        return selftest(w)
    if not args.dumps:
        ap.error("a ring dump is required (or --selftest)")

    for path in args.dumps:
        words = decode_cs.read_od_ring(path)
        print(f"# {path}: {len(words)} dwords")
        if len(words) > 3:
            print(f"# first three dwords, the ring's own pointers: rptr 0x{words[0]:08x} "
                  f"wptr 0x{words[1]:08x} driver wptr 0x{words[2]:08x}")
        print("# a ring is circular: this is history, several submissions deep\n")
        out = []
        counts = w.walk(words[3:], out, skip_nops=not args.nops)
        print("\n".join(out))
        print("\n  packets: " + ", ".join(f"{k} x{v}" for k, v in
                                          sorted(counts.items(), key=lambda kv: -kv[1])))
        if w.dec.unknown_regs:
            print(f"  {w.dec.unknown_regs} register addresses had no name")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
