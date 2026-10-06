#!/usr/bin/env python3
"""L41: does the AA01 codec answer the standard HDA ELD verbs? Reads only (GET verbs) through the ALSA hwdep
device. HDA_IOCTL_VERB_WRITE = _IOWR('H', 0x11, struct hda_verb_ioctl {u32 verb; u32 res;}); the verb word is
nid << 24 | verb << 8 | payload, as in alsa-tools hda-verb (the kernel adds the codec address)."""
import fcntl, glob, struct

IOC = (3 << 30) | (8 << 16) | (ord('H') << 8) | 0x11


def verb(fd, nid, v, p=0):
    buf = bytearray(struct.pack('<II', (nid << 24) | (v << 8) | p, 0))
    fcntl.ioctl(fd, IOC, buf, True)
    return struct.unpack('<II', buf)[1]


for dev in sorted(glob.glob('/dev/snd/hwC*D*')):
    with open(dev, 'rb+', buffering=0) as f:
        fd = f.fileno()
        vid = verb(fd, 0, 0xF00, 0x00)
        print(f'{dev}: vendor/device 0x{vid:08x} revision 0x{verb(fd, 0, 0xF00, 0x02):08x}')
        if vid >> 16 != 0x1002:
            continue
        sub = verb(fd, 1, 0xF00, 0x04)
        start, count = (sub >> 16) & 0xFF, sub & 0xFF
        print(f'  AFG nodes {start}..{start + count - 1}')
        for nid in range(start, start + count):
            caps = verb(fd, nid, 0xF00, 0x09)
            wtype = (caps >> 20) & 0xF
            if wtype != 4:  # pin complex only
                continue
            pincap = verb(fd, nid, 0xF00, 0x0C)
            cfg = verb(fd, nid, 0xF1C, 0)
            sense = verb(fd, nid, 0xF09, 0)
            print(f'  pin 0x{nid:02x}: widget caps 0x{caps:08x} pin caps 0x{pincap:08x} config 0x{cfg:08x} '
                  f'sense 0x{sense:08x} (presence {sense >> 31 & 1}, ELD valid {sense >> 30 & 1})')
            size = verb(fd, nid, 0xF2E, 0x08)  # DIP size, bit 3 = ELD buffer size
            print(f'    F2E ELD buffer size: 0x{size:08x}')
            n = size & 0xFF
            if 0 < n <= 256:
                data = []
                for i in range(n):
                    r = verb(fd, nid, 0xF2F, i)
                    data.append(r & 0xFF if r >> 31 & 1 else -1)
                valid = sum(1 for d in data if d >= 0)
                print(f'    F2F ELD bytes ({valid}/{n} valid): ' + ' '.join('--' if d < 0 else f'{d:02x}' for d in data))
            for v, name in ((0xF2D, 'DIP index'), (0xF31, 'DIP xmitctrl'), (0xF34, 'conv channel count')):
                try:
                    print(f'    {name} (0x{v:03x}): 0x{verb(fd, nid, v, 0):08x}')
                except OSError as e:
                    print(f'    {name}: {e}')
