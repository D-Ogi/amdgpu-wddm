#!/usr/bin/env python3
"""L1007b: the Azalia endpoint 0 indirect space (ixAZF0ENDPOINT0_*, dcn_2_0_1_offset.h, indices 0x00..0x69) through
mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX (BAR5+0x0E118) and _DATA (BAR5+0x0E11C), regcalc 2026-10-07, the way
dce_audio.c read_indirect_azalia_reg does it: write INDEX, read DATA. The INDEX write selects and changes nothing else;
the old INDEX value is put back at the end. Reference for the Windows r19 endpoint (converter format, stream id,
digital converter, LPIB) while a tone plays."""
import glob, os, struct, sys, time

IDX, DATA = 0x0E118, 0x0E11C
path = sorted(glob.glob('/sys/kernel/debug/dri/*/amdgpu_regs'))[0]
fd = os.open(path, os.O_RDWR)
rd = lambda off: struct.unpack('<I', os.pread(fd, 4, off))[0]
wr = lambda off, v: os.pwrite(fd, struct.pack('<I', v), off)
old = rd(IDX)
print(f'# {sys.argv[1] if len(sys.argv) > 1 else ""} INDEX was 0x{old:08x}')
for ix in list(range(0x00, 0x0F)) + list(range(0x20, 0x26)) + list(range(0x36, 0x39)) + list(range(0x54, 0x59)) + list(range(0x62, 0x6A)):
    wr(IDX, ix)
    print(f'ix 0x{ix:02x} 0x{rd(DATA):08x}')
# LPIB rate: two snapshots 1 s apart (ix 0x65 LPIB, 0x66 timer snapshot)
wr(IDX, 0x65); a = rd(DATA); t0 = time.monotonic(); time.sleep(1.0); wr(IDX, 0x65); b = rd(DATA); t1 = time.monotonic()
print(f'LPIB 0x65: {a} -> {b} in {t1 - t0:.3f} s')
wr(IDX, old)
