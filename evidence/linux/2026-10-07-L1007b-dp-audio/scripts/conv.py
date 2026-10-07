#!/usr/bin/env python3
"""L1007b: the AA01 codec's converter and pin state, GET verbs only, through the ALSA hwdep device (as L1007 verbs.py).
Run during and after a playback: F0D digital converter control (DigEn bit 0, the IEC958 Playback Switch), A00 stream
format, F06 stream/channel, F2D converter channel count, F07 pin widget control, F05 power state, for every node."""
import fcntl, glob, struct, sys

IOC = (3 << 30) | (8 << 16) | (ord('H') << 8) | 0x11


def verb(fd, nid, v, p=0):
    buf = bytearray(struct.pack('<II', (nid << 24) | (v << 8) | p, 0))
    fcntl.ioctl(fd, IOC, buf, True)
    return struct.unpack('<II', buf)[1]


print('# ' + (sys.argv[1] if len(sys.argv) > 1 else ''))
dev = sorted(glob.glob('/dev/snd/hwC0D*'))[0]
with open(dev, 'rb+', buffering=0) as f:
    fd = f.fileno()
    for nid in range(1, 6):
        caps = verb(fd, nid, 0xF00, 0x09)
        row = [f'node 0x{nid:02x} type {(caps >> 20) & 0xF} caps 0x{caps:08x}']
        for v, name in ((0xF0D, 'F0D digconv'), (0xA00, 'A00 format'), (0xF06, 'F06 stream/ch'), (0xF2D, 'F2D convch'),
                        (0xF07, 'F07 pinctl'), (0xF05, 'F05 power')):
            row.append(f'{name} 0x{verb(fd, nid, v, 0):08x}')
        print('  '.join(row))
