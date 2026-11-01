#!/usr/bin/env python3
"""L1031: read named registers through amdgpu's debugfs register file (reads only).
Table lines: '<NAME> 0x<byte offset>' generated on the development PC by tools/regcalc (dcn_2_0_1_offset.h,
gc_10_1_0_offset.h, mmhub_2_0_0_offset.h). Names with LUT_DATA, _DATA or _INDEX were left out of the tables
(indexed data ports advance on read). Usage: regread.py <table> [label]"""
import glob, os, struct, sys

path = sorted(glob.glob('/sys/kernel/debug/dri/*/amdgpu_regs'))[0]
fd = os.open(path, os.O_RDONLY)
print(f'# {sys.argv[2] if len(sys.argv) > 2 else ""} {path} table={sys.argv[1]}')
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) != 2:
        continue
    name, off = parts[0], int(parts[1], 16)
    try:
        v = struct.unpack('<I', os.pread(fd, 4, off))[0]
        print(f'{name:56s} 0x{off:05X} 0x{v:08X}')
    except OSError as e:
        print(f'{name:56s} 0x{off:05X} ERR {e}')
