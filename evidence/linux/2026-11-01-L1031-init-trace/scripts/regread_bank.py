#!/usr/bin/env python3
"""L32 per shader array: read a register table through amdgpu_regs with the debugfs bank-select offset
(bit 62; SE in bits 24-33, SH in 34-43, instance in 44-53; 0x3FF = broadcast). The kernel selects the bank under
grbm_idx_mutex and restores broadcast after the read, so no GRBM_GFX_INDEX write is left behind.
Usage: regread_bank.py <table> [max_se] [max_sh]"""
import glob, os, struct, sys

path = sorted(glob.glob('/sys/kernel/debug/dri/*/amdgpu_regs'))[0]
fd = os.open(path, os.O_RDONLY)
table = [l.split() for l in open(sys.argv[1]) if len(l.split()) == 2]
max_se = int(sys.argv[2]) if len(sys.argv) > 2 else 2
max_sh = int(sys.argv[3]) if len(sys.argv) > 3 else 2
BC = 0x3FF


def pos(off, se, sh, inst=BC):
    return (1 << 62) | (se << 24) | (sh << 34) | (inst << 44) | off


banks = [("broadcast", BC, BC)] + [(f"se{se}sh{sh}", se, sh) for se in range(max_se) for sh in range(max_sh)]
print(f"# {path} table={sys.argv[1]} banks={','.join(b[0] for b in banks)}")
print(f"{'name':40s} {'off':7s} " + " ".join(f"{b[0]:>10s}" for b in banks))
for name, off in table:
    o = int(off, 16)
    vals = []
    for _, se, sh in banks:
        try:
            vals.append(f"0x{struct.unpack('<I', os.pread(fd, 4, pos(o, se, sh)))[0]:08X}")
        except OSError as e:
            vals.append(f"ERR{e.errno}")
    print(f"{name:40s} 0x{o:05X} " + " ".join(f"{v:>10s}" for v in vals))
