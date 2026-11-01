#!/usr/bin/env python3
"""L35, on the development PC after the pull: compare the unit A reads with the stock P3.00 image and with the
E30 AmdSetup payload. Prints offsets, lengths and counts only, never image bytes.

  python -I l35-compare.py <dir with spi-read1.bin spi-read2.bin> [stock image] [AmdSetup.payload.bin]
Defaults: P:/BC-250/firmware/bios/P3.00-stock/BC250_3.00.ROM and
          P:/BC-250/firmware/bios/unit-a/E30-20260924/AmdSetup.payload.bin"""
import hashlib
import sys
from pathlib import Path

ROOT = Path('P:/BC-250/firmware/bios')
d = Path(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(__doc__)
stock = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'P3.00-stock/BC250_3.00.ROM'
setup = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / 'unit-a/E30-20260924/AmdSetup.payload.bin'
a, b, s = (d / 'spi-read1.bin').read_bytes(), (d / 'spi-read2.bin').read_bytes(), stock.read_bytes()
print(f'read1 {len(a)} {hashlib.sha256(a).hexdigest()}')
print(f'read2 {len(b)} {hashlib.sha256(b).hexdigest()}  identical={a == b}')
print(f'stock {len(s)} {hashlib.sha256(s).hexdigest()}  ({stock.name})')


def ranges(x, y, gran=4096):
    out, start = [], None
    for off in range(0, min(len(x), len(y)), gran):
        diff = x[off:off + gran] != y[off:off + gran]
        if diff and start is None:
            start = off
        if not diff and start is not None:
            out.append((start, off - start))
            start = None
    if start is not None:
        out.append((start, len(x) - start))
    return out


r = ranges(a, s)
print(f'unit A vs stock: {len(r)} differing ranges, {sum(n for _, n in r)} bytes at 4 KiB granularity')
for off, n in r[:60]:
    print(f'  0x{off:07X} +0x{n:X}')
if setup.exists():
    p = setup.read_bytes()
    hits, i = [], a.find(p[:64])
    while i >= 0 and len(hits) < 4:
        hits.append(i)
        i = a.find(p[:64], i + 1)
    print(f'AmdSetup payload ({len(p)} bytes): first-64-byte matches at {[hex(h) for h in hits]}; '
          f'full payload present: {any(a[h:h + len(p)] == p for h in hits)}')
