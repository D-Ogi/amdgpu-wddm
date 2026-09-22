#!/usr/bin/env python3
"""How much stack does each function in a built driver allocate? Read it out of the image itself.

    python stackbudget.py bc250kmd.sys [--map bc250kmd.map] [--warn 1024] [--fail 4096]

A kernel thread gets 24 KB of stack (x64) and the driver is never at the bottom of it: when dxgmms2
calls DdiBuildPagingBuffer, eight of its own frames are already there. E24 run 004 bugchecked 0x50 in
nt!_chkstk because one shim function declared a `struct amdgpu_device` (0x5B00 bytes) as a local -
facts M104. Nothing in the compiler's default warning set says a word about that, so this does.

x64 needs no disassembler for the question: every non-leaf function's prologue is described in
.pdata/.xdata (RUNTIME_FUNCTION -> UNWIND_INFO), and the unwind codes spell out exactly how many bytes
the prologue subtracts from rsp. This walks that table. Dynamic allocation (alloca, a variable-length
array) is invisible here - it is not in the prologue - but the fixed frame, which is the kind that
kills a driver, is exact.

Names come from the linker map (/MAP), when one is passed; without it the report carries RVAs, which
`ln <module>+<rva>` in kd turns into names.

For scale, the same walk over Microsoft's own display stack on this machine (Windows 11 26200):
dxgkrnl.sys 8474 functions, largest fixed frame 4296 bytes; dxgmms2.sys 2414 functions, largest 2808.
Those two are at the bottom of the stack when they call us, so a miniport wants to stay well under them;
0.7.26's largest is 872.

Exit code 1 if any function is at or over --fail, so a build script can stop there.
"""

import argparse
import os
import struct
import sys

UWOP_PUSH_NONVOL = 0
UWOP_ALLOC_LARGE = 1
UWOP_ALLOC_SMALL = 2
UWOP_SET_FPREG = 3
UWOP_SAVE_NONVOL = 4
UWOP_SAVE_NONVOL_FAR = 5
UWOP_SAVE_XMM128 = 8
UWOP_SAVE_XMM128_FAR = 9
UWOP_PUSH_MACHFRAME = 10

UNW_FLAG_CHAININFO = 0x4

# How many extra 2-byte slots the code's operand takes, beyond the code itself.
EXTRA_SLOTS = {UWOP_SAVE_NONVOL: 1, UWOP_SAVE_NONVOL_FAR: 2, UWOP_SAVE_XMM128: 1, UWOP_SAVE_XMM128_FAR: 2}


class Image(object):
    """Just enough PE to turn an RVA into bytes."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s is not a PE image" % path)
        sections = struct.unpack_from("<H", self.data, pe + 6)[0]
        opt = struct.unpack_from("<H", self.data, pe + 20)[0]
        magic = struct.unpack_from("<H", self.data, pe + 24)[0]
        if magic != 0x20B:
            raise ValueError("%s is not PE32+ (x64): this reads x64 unwind data only" % path)
        self.base = struct.unpack_from("<Q", self.data, pe + 24 + 24)[0]
        # Data directory 3 is the exception directory: the RUNTIME_FUNCTION table.
        dd = pe + 24 + 112
        self.pdata_rva, self.pdata_size = struct.unpack_from("<II", self.data, dd + 3 * 8)
        self.sections = []
        first = pe + 24 + opt
        for i in range(sections):
            off = first + i * 40
            name = self.data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", self.data, off + 8)
            self.sections.append((name, vaddr, max(vsize, rawsize), rawptr))

    def at(self, rva, size):
        for _name, vaddr, vsize, rawptr in self.sections:
            if vaddr <= rva < vaddr + vsize:
                off = rawptr + (rva - vaddr)
                return self.data[off:off + size]
        raise ValueError("rva 0x%X is in no section" % rva)


def frame_bytes(img, unwind_rva, seen=None, root=None):
    """Bytes the prologue described by this UNWIND_INFO takes off rsp, chained records included.

    Returns (bytes, function start rva). The compiler splits one function into several
    RUNTIME_FUNCTIONs whose unwind records chain back to the first; following the chain to its end is
    what turns those fragments back into the one function the source declared.
    """
    seen = seen or set()
    if unwind_rva in seen:                      # a malformed chain must not become an infinite loop
        return 0, root

    head = img.at(unwind_rva, 4)
    flags = head[0] >> 3
    count = head[2]
    codes = img.at(unwind_rva + 4, count * 2)

    total, i = 0, 0
    while i < count:
        op = codes[i * 2 + 1] & 0xF
        info = codes[i * 2 + 1] >> 4
        if op == UWOP_PUSH_NONVOL:
            total += 8
        elif op == UWOP_ALLOC_LARGE:
            if info == 0:
                total += struct.unpack_from("<H", codes, (i + 1) * 2)[0] * 8
                i += 1
            else:
                total += struct.unpack_from("<I", codes, (i + 1) * 2)[0]
                i += 2
        elif op == UWOP_ALLOC_SMALL:
            total += (info + 1) * 8
        elif op == UWOP_PUSH_MACHFRAME:
            total += 48 if info else 40
        else:
            i += EXTRA_SLOTS.get(op, 0)
        i += 1

    if flags & UNW_FLAG_CHAININFO:
        # The chained RUNTIME_FUNCTION follows the codes, which are padded to an even count.
        at = unwind_rva + 4 + ((count + 1) & ~1) * 2
        begin, _end, parent = struct.unpack("<III", img.at(at, 12))
        more, root = frame_bytes(img, parent, seen, begin)
        total += more
    return total, root


def read_map(path, base):
    """[(rva, name)] from a linker /MAP file, sorted - the map's third column is the load address."""
    out = []
    for line in open(path, "r", errors="replace"):
        parts = line.split()
        if len(parts) < 3 or ":" not in parts[0]:
            continue
        try:
            va = int(parts[2], 16)
        except ValueError:
            continue
        if va >= base:
            out.append((va - base, parts[1]))
    out.sort()
    return out


def name_of(symbols, rva):
    """The last symbol at or before rva; binary search, since a driver has thousands."""
    lo, hi = 0, len(symbols)
    while lo < hi:
        mid = (lo + hi) // 2
        if symbols[mid][0] <= rva:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return "+0x%X" % rva
    at, name = symbols[lo - 1]
    return name if at == rva else "%s+0x%X" % (name, rva - at)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--map", dest="map_file", default=None, help="linker /MAP file, for names")
    ap.add_argument("--warn", type=int, default=1024, help="list every function at or over this many bytes")
    ap.add_argument("--fail", type=int, default=4096, help="exit 1 if any function is at or over this")
    args = ap.parse_args(argv)

    img = Image(args.image)
    if args.map_file is None:
        guess = os.path.splitext(args.image)[0] + ".map"
        if os.path.exists(guess):
            args.map_file = guess
    symbols = read_map(args.map_file, img.base) if args.map_file else []

    peak = {}
    for off in range(0, img.pdata_size, 12):
        begin, _end, unwind = struct.unpack("<III", img.at(img.pdata_rva + off, 12))
        if unwind == 0 or (unwind & 1):         # odd means "this is itself a chain pointer", not our concern
            continue
        try:
            size, root = frame_bytes(img, unwind, None, begin)
        except ValueError:
            continue
        at = root if root is not None else begin
        if size > peak.get(at, -1):
            peak[at] = size

    entries = sorted(((size, at) for at, size in peak.items()), reverse=True)
    over = [e for e in entries if e[0] >= args.warn]
    worst = entries[0][0] if entries else 0

    print("%s: %d functions with unwind data, largest fixed frame %d bytes" % (
        os.path.basename(args.image), len(entries), worst))
    if not symbols and args.map_file:
        print("  (no symbols read from %s)" % args.map_file)
    for size, rva in over:
        mark = "FAIL" if size >= args.fail else "warn"
        print("  %-4s %7d bytes  rva 0x%06X  %s" % (mark, size, rva, name_of(symbols, rva) if symbols else ""))
    if not over:
        print("  nothing at or over %d bytes" % args.warn)

    bad = [e for e in entries if e[0] >= args.fail]
    if bad:
        print("stack budget: %d function(s) at or over %d bytes - a kernel thread has 24576" % (len(bad), args.fail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
