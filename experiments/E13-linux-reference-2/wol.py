#!/usr/bin/env python3
"""Turn wake-on-LAN (magic packet) on for one interface before leaving Linux, as `ethtool -s eth0 wol g` would (the
stick has no ethtool). Why: with wake-on-LAN off, r8169 powers the PHY down at shutdown, and after a warm restart
Windows came up with the wired NIC "Not Present" (E13, journal 2026-09-21; KDNET sits on that NIC). With it on, r8169
leaves the PHY powered. Volatile NIC registers only: nothing is written to an EEPROM, the firmware or NVRAM.

    python3 wol.py eth0          show, set magic-packet wake, show again

Constants from the kernel's UAPI (include/uapi/linux/sockios.h, ethtool.h): SIOCETHTOOL 0x8946, ETHTOOL_GWOL 5,
ETHTOOL_SWOL 6, WAKE_MAGIC bit 5. struct ethtool_wolinfo: u32 cmd, supported, wolopts; u8 sopass[6].
"""

import fcntl
import socket
import struct
import sys
import array

SIOCETHTOOL, GWOL, SWOL, WAKE_MAGIC = 0x8946, 5, 6, 1 << 5


def call(sock, name, cmd, wolopts=0):
    info = array.array("B", struct.pack("<III6s2x", cmd, 0, wolopts, b""))
    address, _ = info.buffer_info()
    fcntl.ioctl(sock, SIOCETHTOOL, struct.pack("16sP", name.encode(), address))
    return struct.unpack("<III", info.tobytes()[:12])


def main():
    name = sys.argv[1]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    _, supported, now = call(s, name, GWOL)
    print(f"{name}: supported 0x{supported:x}, wake options 0x{now:x}")
    if not supported & WAKE_MAGIC:
        sys.exit("magic-packet wake is not supported here")
    call(s, name, SWOL, WAKE_MAGIC)
    _, _, now = call(s, name, GWOL)
    print(f"{name}: wake options now 0x{now:x}")


if __name__ == "__main__":
    main()
