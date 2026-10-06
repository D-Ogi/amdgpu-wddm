#!/usr/bin/env python3
"""L41 part 2: the ATI vendor GET verbs that Linux atihdmi.c uses for the ELD (v6.18 lines 62-71). GET only."""
import fcntl, struct
IOC = (3 << 30) | (8 << 16) | (ord('H') << 8) | 0x11
def verb(fd, nid, v, p=0):
    buf = bytearray(struct.pack('<II', (nid << 24) | (v << 8) | p, 0)); fcntl.ioctl(fd, IOC, buf, True); return struct.unpack('<II', buf)[1]
with open('/dev/snd/hwC0D0', 'rb+', buffering=0) as f:
    fd = f.fileno()
    for nid in (0x03, 0x05):
        print(f'pin 0x{nid:02x}')
        for v, name in ((0xF70, 'GET_SPEAKER_ALLOCATION'), (0xF76, 'GET_AUDIO_DESCRIPTOR'), (0xF7B, 'GET_AUDIO_VIDEO_DELAY'),
                        (0xF80, 'GET_SINK_INFO_INDEX'), (0xF81, 'GET_SINK_INFO_DATA'), (0xF7C, 'GET_HBR_CONTROL'),
                        (0xF89, 'GET_MULTICHANNEL_MODE'), (0xF09, 'pin sense'), (0xF07, 'pin widget control'), (0xF08, 'unsol enable')):
            print(f'  {name:24s} 0x{v:03x}: 0x{verb(fd, nid, v, 0):08x}')
