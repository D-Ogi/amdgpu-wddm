#!/usr/bin/env python3
"""L1007b: the HD Audio controller's wall clock (HDA spec 1.0a, 3.3.16 WALCLK at offset 0x30 of the controller BAR, a
24 MHz counter) on the GPU's audio function, read twice 1 s apart through the PCI resource file (reads only). The
positive control for the Windows r19 0.33x: on Linux, where audio plays at 1.0x, the counter must advance 24e6/s."""
import glob, mmap, os, struct, time

dev = [d for d in sorted(glob.glob('/sys/bus/pci/devices/*')) if open(d + '/vendor').read().strip() == '0x1002'
       and open(d + '/class').read().strip().startswith('0x0403')][0]
fd = os.open(dev + '/resource0', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ)
rd = lambda off: struct.unpack('<I', m[off:off + 4])[0]
gcap, vmin, vmaj = rd(0x00) & 0xFFFF, rd(0x00) >> 16 & 0xFF, rd(0x00) >> 24
print(f'{dev}: GCAP 0x{gcap:04x} version {vmaj}.{vmin}')
w0, t0 = rd(0x30), time.monotonic()
time.sleep(1.0)
w1, t1 = rd(0x30), time.monotonic()
rate = ((w1 - w0) & 0xFFFFFFFF) / (t1 - t0)
print(f'WALCLK {w0} -> {w1} in {t1 - t0:.4f} s: {rate / 1e6:.4f} MHz (24.0000 expected)')
for s in range(4):  # output stream descriptors follow the input ones: base 0x80 + 0x20 * (ISS + n)
    iss = gcap >> 8 & 0xF
    b = 0x80 + 0x20 * (iss + s)
    ctl, lpib, cbl, fmt = rd(b) & 0xFFFFFF, rd(b + 4), rd(b + 8), rd(b + 0x10) & 0xFFFF
    print(f'out stream {s}: CTL 0x{ctl:06x} LPIB {lpib} CBL {cbl} FMT 0x{fmt:04x}')
