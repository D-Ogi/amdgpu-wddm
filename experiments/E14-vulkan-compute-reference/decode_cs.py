#!/usr/bin/env python3
"""decode_cs.py - decode the PM4 command stream RADV submits for one compute dispatch.

Runs on the development PC, not on the lab. Input is the plain-text dump that
`RADV_DEBUG=dumpibs` writes to stderr (E14's run-cs.sh strips the ANSI colour first). Only the
raw dword column of that dump is read; every name printed here is derived independently, so the
output is our own decode and ac_debug's text next to it is a control, not a source.

Where the names come from, so that nothing is typed by hand:

  PKT3 opcode names   `#define PKT3_<NAME> 0x<op>` in a reference header. Mesa's
                      src/amd/common/sid.h by default; the kernel's amdgpu nvd.h understands the
                      same opcodes under the name PACKET3_<NAME> and can be used with --pkt-header.
  register windows    SI_SH_REG_OFFSET / SI_CONTEXT_REG_OFFSET / CIK_UCONFIG_REG_OFFSET, read
                      from the same header. A SET_*_REG packet carries a dword index relative to
                      its window, so the BAR5 byte offset is window + (index << 2).
  register names      tools/regcalc, which computes them from the AMD headers. No mm* offset is
                      written down in this file.

That last step was checked against ac_debug's own naming before this decoder was used for
anything: SET_SH_REG offsets 0x020c, 0x0212, 0x0228, 0x0215, 0x0207 and 0x0242 come back as
mmCOMPUTE_PGM_LO, mmCOMPUTE_PGM_RSRC1, mmCOMPUTE_PGM_RSRC3, mmCOMPUTE_RESOURCE_LIMITS,
mmCOMPUTE_NUM_THREAD_X and mmCOMPUTE_USER_DATA_2, which is what Mesa prints for the same packets.
`--selftest` repeats that check.

Usage:
    decode_cs.py <ib-dump.txt> [--pkt-header <sid.h|nvd.h>] [--hex] [--selftest]
    decode_cs.py --selftest
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "regcalc"))
import regcalc  # noqa: E402

WORKSPACE = REPO.parent
MESA_SID = WORKSPACE / "ref" / "mesa" / "src" / "amd" / "common" / "sid.h"
KERNEL_NVD = WORKSPACE / "ref" / "linux-src" / "drivers" / "gpu" / "drm" / "amd" / "amdgpu" / "nvd.h"

# A dword of the dump: eight hex digits at the start of the line. Everything else on the line is
# ac_debug's commentary and is deliberately ignored.
_DWORD_RE = re.compile(r"^([0-9a-f]{8})(?:\s|$)")
_IB_RE = re.compile(r"^-{3,}\s*(.+?)\s+(begin|end)\s*-\s*(\w+)\s*-{3,}$")
_PKT3_RE = re.compile(r"^#define\s+(?:PKT3|PACKET3)_([A-Z0-9_]+)\s+0x([0-9A-Fa-f]+)")
# The two reference headers state the SET_*_REG windows differently and both are accepted:
# Mesa's sid.h gives a BYTE offset (SI_SH_REG_OFFSET 0x0000B000), the kernel's nvd.h the same
# window as a DWORD index (PACKET3_SET_SH_REG_START 0x00002c00). They agree, which is a useful
# check in itself; everything below works in bytes.
_WINDOW_RE = re.compile(r"^#define\s+(SI_SH_REG_OFFSET|SI_CONTEXT_REG_OFFSET|CIK_UCONFIG_REG_OFFSET)"
                        r"\s+0x([0-9A-Fa-f]+)")
_WINDOW_START_RE = re.compile(r"^#define\s+PACKET3_SET_(SH|CONTEXT|UCONFIG)_REG_START"
                              r"\s+0x([0-9A-Fa-f]+)")
_WINDOW_KEY = {"SH": "SI_SH_REG_OFFSET", "CONTEXT": "SI_CONTEXT_REG_OFFSET",
               "UCONFIG": "CIK_UCONFIG_REG_OFFSET"}


def load_header(path):
    """PKT3 opcode names and the three SET_*_REG window bases, from a reference header."""
    ops, windows = {}, {}
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        # Windows first: in nvd.h they are spelled PACKET3_SET_*_REG_START, which the opcode
        # pattern below would otherwise swallow.
        m = _WINDOW_RE.match(line)
        if m:
            windows[m.group(1)] = int(m.group(2), 16)
            continue
        m = _WINDOW_START_RE.match(line)
        if m:
            windows.setdefault(_WINDOW_KEY[m.group(1)], int(m.group(2), 16) << 2)
            continue
        m = _PKT3_RE.match(line)
        if m:
            # Several names can share an opcode across generations; keep the first, which in both
            # headers is the current one. Names ending in _START/_END are window markers, not ops.
            name, op = m.group(1), int(m.group(2), 16)
            if not name.endswith(("_START", "_END")) and op not in ops:
                ops[op] = name
    missing = set(_WINDOW_KEY.values()) - set(windows)
    if missing:
        raise SystemExit(f"{path}: no SET_*_REG window bases for {sorted(missing)}; "
                         f"this header cannot be used")
    return ops, windows


_OD_RE = re.compile(r"^([0-9a-f]+)\s+((?:[0-9a-f]{8}\s*)+)$")


def read_od_ring(path):
    """Dwords of a ring dump taken with `dd ... | od -A x -t x4 -v`."""
    words = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = _OD_RE.match(line.strip())
        if m:
            words.extend(int(w, 16) for w in m.group(2).split())
    return words


def scan_ring(words, ops, out):
    """Find the INDIRECT_BUFFER packets a ring holds: what the CP was pointed at, in order.

    A ring is a circular buffer of whatever was submitted before, so this is history, not one
    submission. Only the packet kind we are sure of is reported; the rest is left as raw dwords
    rather than pretended to be decoded.
    """
    ib_op = next((op for op, n in ops.items() if n == "INDIRECT_BUFFER"), None)
    if ib_op is None:
        out.append("  the header has no INDIRECT_BUFFER opcode")
        return 0
    found = 0
    for i in range(len(words) - 3):
        hdr = words[i]
        if (hdr >> 30) != 3 or ((hdr >> 8) & 0xFF) != ib_op or ((hdr >> 16) & 0x3FFF) != 2:
            continue
        lo, hi, ctl = words[i + 1], words[i + 2], words[i + 3]
        va = (hi << 32) | (lo & 0xFFFFFFFC)
        size, chain, vmid = ctl & 0xFFFFF, (ctl >> 20) & 1, (ctl >> 24) & 0xF
        # A plausible IB: non-zero canonical VA and a size that fits a ring-sized buffer.
        if va == 0 or size == 0 or size > 0x40000:
            continue
        found += 1
        out.append(f"  [dword {i:5d} / byte 0x{i * 4:05x}]  INDIRECT_BUFFER  "
                   f"VA=0x{va:016x}  size={size} dwords ({size * 4} bytes)  "
                   f"VMID={vmid} CHAIN={chain}")
    return found


def read_dumped_ibs(path):
    """[(label, [dwords])] in submission order, from an ANSI-stripped dumpibs dump."""
    ibs, label, words = [], None, []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = _IB_RE.match(line.strip())
        if m:
            what, edge, ring = m.group(1), m.group(2), m.group(3)
            if edge == "begin":
                label, words = f"{what} [{ring}]", []
            else:
                if label is not None:
                    ibs.append((label, words))
                label, words = None, []
            continue
        m = _DWORD_RE.match(line)
        if m and label is not None:
            words.append(int(m.group(1), 16))
    if label is not None and words:
        ibs.append((label, words))
    return ibs


class Decoder:
    def __init__(self, ops, windows, regmap):
        self.ops = ops
        self.regmap = regmap
        self.sh = windows["SI_SH_REG_OFFSET"]
        self.ctx = windows["SI_CONTEXT_REG_OFFSET"]
        self.uconfig = windows["CIK_UCONFIG_REG_OFFSET"]
        self.unknown_regs = 0

    def reg_name(self, window, index):
        """Register at `index` dwords into `window`, named by regcalc. Never a typed offset."""
        byte = window + (index << 2)
        names = self.regmap.reverse(byte)
        if not names:
            self.unknown_regs += 1
            return f"<no GC register at BAR5+0x{byte:05X}>", byte
        return "/".join(names), byte

    def window_for(self, opname):
        if opname.startswith("SET_SH_REG"):
            return self.sh
        if opname.startswith("SET_CONTEXT_REG"):
            return self.ctx
        if opname.startswith("SET_UCONFIG_REG"):
            return self.uconfig
        return None

    def decode(self, words, out, show_hex):
        i, n = 0, len(words)
        counts = {}
        while i < n:
            hdr = words[i]
            typ = hdr >> 30
            if typ == 3:
                count = (hdr >> 16) & 0x3FFF
                op = (hdr >> 8) & 0xFF
                total = count + 2
                body = words[i + 1:i + total]
                name = self.ops.get(op, f"UNKNOWN_OP_0x{op:02X}")
                flags = []
                if hdr & 0x2:
                    flags.append("shader_type=compute")
                if hdr & 0x1:
                    flags.append("predicate")
                counts[name] = counts.get(name, 0) + 1
                out.append(f"  [{i:4d}] {hdr:08x}  PKT3 {name}"
                           f"{' (' + ', '.join(flags) + ')' if flags else ''}"
                           f"  count={count} dwords={total}")
                self._body(name, body, out, show_hex)
                i += total
            elif typ == 2:
                counts["TYPE2_NOP"] = counts.get("TYPE2_NOP", 0) + 1
                out.append(f"  [{i:4d}] {hdr:08x}  TYPE2 filler NOP")
                i += 1
            elif typ == 0:
                count = (hdr >> 16) & 0x3FFF
                total = count + 2
                counts["TYPE0"] = counts.get("TYPE0", 0) + 1
                out.append(f"  [{i:4d}] {hdr:08x}  TYPE0 reg write, count={count}")
                i += total
            else:
                out.append(f"  [{i:4d}] {hdr:08x}  TYPE1 (reserved) - stopping")
                break
        return counts

    def _body(self, name, body, out, show_hex):
        window = self.window_for(name)
        if window is not None and body:
            index = body[0] & 0xFFFF
            for k, val in enumerate(body[1:]):
                rname, byte = self.reg_name(window, index + k)
                out.append(f"          {val:08x}    {rname}  (BAR5+0x{byte:05X}) <- 0x{val:08x}")
            if len(body) == 1:
                out.append("          (no values)")
            return

        if name == "INDIRECT_BUFFER" and len(body) >= 3:
            # IB_BASE_HI carries the full upper 32 bits: the VA is a 48-bit canonical address
            # sign-extended into bits 63:48, so it reads back as 0xffff8001_xxxxxxxx here.
            va = (body[1] << 32) | (body[0] & 0xFFFFFFFC)
            size = body[2] & 0xFFFFF
            chain = (body[2] >> 20) & 1
            vmid = (body[2] >> 24) & 0xF
            out.append(f"          IB_BASE_LO={body[0]:08x} IB_BASE_HI={body[1]:08x}")
            out.append(f"          IB_VA=0x{va:016x} IB_SIZE={size} dwords "
                       f"({size * 4} bytes) VMID={vmid} CHAIN={chain}")
            return

        if name == "DISPATCH_DIRECT" and len(body) >= 4:
            # The last dword goes to COMPUTE_DISPATCH_INITIATOR. Its address comes from regcalc by
            # name, not from arithmetic on the SH window: the packet does not carry an offset here.
            rname = "mmCOMPUTE_DISPATCH_INITIATOR"
            byte = self.regmap.byte_offset(rname) if rname in self.regmap.regs else None
            out.append(f"          DIM_X={body[0]} DIM_Y={body[1]} DIM_Z={body[2]}")
            where = f"(BAR5+0x{byte:05X})" if byte is not None else "(not in header)"
            out.append(f"          {body[3]:08x}    {rname}  {where} <- 0x{body[3]:08x}")
            return

        if show_hex:
            for k, val in enumerate(body):
                out.append(f"          {val:08x}    (body[{k}])")


def selftest():
    """The positive control: our register naming must agree with ac_debug's for known packets."""
    ops, windows = load_header(MESA_SID)
    rm = regcalc.RegMap()
    dec = Decoder(ops, windows, rm)
    cases = [
        (0x020C, "mmCOMPUTE_PGM_LO"),
        (0x0212, "mmCOMPUTE_PGM_RSRC1"),
        (0x0228, "mmCOMPUTE_PGM_RSRC3"),
        (0x0215, "mmCOMPUTE_RESOURCE_LIMITS"),
        (0x0207, "mmCOMPUTE_NUM_THREAD_X"),
        (0x0242, "mmCOMPUTE_USER_DATA_2"),
    ]
    ok = True
    print("SET_SH_REG offsets, against the names ac_debug prints for the same packets:")
    for off, expect in cases:
        got, byte = dec.reg_name(dec.sh, off)
        hit = expect in got.split("/")
        ok &= hit
        print(f"  0x{off:04x} -> BAR5+0x{byte:05X}  {got:32s} expect {expect:28s} "
              f"{'OK' if hit else 'MISMATCH'}")
    known_ops = {0x15: "DISPATCH_DIRECT", 0x37: "WRITE_DATA", 0x3F: "INDIRECT_BUFFER",
                 0x28: "CONTEXT_CONTROL", 0x76: "SET_SH_REG"}
    print(f"\nPKT3 opcodes parsed from {MESA_SID.name}: {len(ops)}")
    for op, expect in known_ops.items():
        got = ops.get(op, "<missing>")
        hit = got == expect
        ok &= hit
        print(f"  0x{op:02X} -> {got:20s} expect {expect:20s} {'OK' if hit else 'MISMATCH'}")
    print(f"\nwindows: SH=0x{dec.sh:X} CONTEXT=0x{dec.ctx:X} UCONFIG=0x{dec.uconfig:X}")
    print("\nSELFTEST PASSED" if ok else "\nSELFTEST FAILED")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", nargs="?", help="ANSI-stripped RADV_DEBUG=dumpibs output")
    ap.add_argument("--pkt-header", default=str(MESA_SID),
                    help=f"header with the PKT3 names (default: {MESA_SID})")
    ap.add_argument("--hex", action="store_true", help="also print body dwords of packets we do "
                                                       "not decode field by field")
    ap.add_argument("--ring", action="store_true",
                    help="the input is a ring dump from `dd | od -A x -t x4 -v`, not a dumpibs "
                         "dump: list the INDIRECT_BUFFER packets it still holds")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.dump:
        ap.error("a dump file is required (or --selftest)")

    ops, windows = load_header(args.pkt_header)

    if args.ring:
        words = read_od_ring(args.dump)
        print(f"# {args.dump}: {len(words)} dwords ({len(words) * 4} bytes) of ring")
        print("# a ring is circular and holds earlier submissions too - this is history\n")
        out = []
        found = scan_ring(words, ops, out)
        print("\n".join(out) if out else "  no INDIRECT_BUFFER packet found")
        print(f"\n  {found} INDIRECT_BUFFER packet(s)")
        nonzero = sum(1 for w in words if w)
        print(f"  {nonzero} of {len(words)} dwords are non-zero")
        return 0

    dec = Decoder(ops, windows, regcalc.RegMap())
    ibs = read_dumped_ibs(args.dump)
    if not ibs:
        print(f"no IB sections found in {args.dump}", file=sys.stderr)
        return 2

    print(f"# {args.dump}")
    print(f"# PKT3 names from {args.pkt_header}, register names from tools/regcalc")
    print(f"# {len(ibs)} IB(s) in submission order\n")
    grand = {}
    for label, words in ibs:
        print(f"=== IB: {label} - {len(words)} dwords ({len(words) * 4} bytes) ===")
        out = []
        counts = dec.decode(words, out, args.hex)
        print("\n".join(out))
        for k, v in counts.items():
            grand[k] = grand.get(k, 0) + v
        print()
    print("=== packet totals across all IBs ===")
    for k in sorted(grand, key=lambda x: (-grand[x], x)):
        print(f"  {grand[k]:4d}  {k}")
    if dec.unknown_regs:
        print(f"\n  note: {dec.unknown_regs} register slot(s) had no GC name at that offset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
