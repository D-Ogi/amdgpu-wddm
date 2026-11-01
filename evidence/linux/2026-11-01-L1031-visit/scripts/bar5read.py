#!/usr/bin/env python3
"""L7: read named GPU registers through BAR5 (sysfs resource5) BEFORE amdgpu binds, 32-bit loads only.
The BAR is mapped PROT_READ through libc mmap; there is no write path in this file.
Usage: bar5read.py <table> [label]. Table lines '<NAME> 0x<BAR5 byte offset>' from tools/regcalc.
Refuses when a driver is bound to 0000:01:00.0 (then use regread.py through debugfs)."""
import ctypes
import os
import sys

DEV = '/sys/bus/pci/devices/0000:01:00.0'


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if os.path.exists(DEV + '/driver'):
        sys.exit('refused: a driver is bound to 01:00.0 (use regread.py)')
    if open(DEV + '/vendor').read().strip() != '0x1002' or open(DEV + '/device').read().strip() != '0x13fe':
        sys.exit('refused: 01:00.0 is not 1002:13fe')
    start, end, _ = (int(x, 16) for x in open(DEV + '/resource').read().splitlines()[5].split())
    size = end - start + 1
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    fd = os.open(DEV + '/resource5', os.O_RDONLY | os.O_SYNC)
    PROT_READ, MAP_SHARED = 1, 1
    base = libc.mmap(None, size, PROT_READ, MAP_SHARED, fd, 0)
    os.close(fd)
    if base in (None, ctypes.c_void_p(-1).value):
        sys.exit(f'mmap failed errno {ctypes.get_errno()}')
    print(f'# {sys.argv[2] if len(sys.argv) > 2 else ""} BAR5 phys 0x{start:x} size 0x{size:x} PROT_READ table={sys.argv[1]}')
    for line in open(sys.argv[1]):
        p = line.split('#')[0].split()
        if len(p) != 2:
            continue
        name, off = p[0], int(p[1], 16)
        if off % 4 or off + 4 > size:
            print(f'{name:48s} 0x{off:05X} OUT-OF-BAR')
            continue
        v = ctypes.c_uint32.from_address(base + off).value
        print(f'{name:48s} 0x{off:05X} 0x{v:08X}')


if __name__ == '__main__':
    main()
