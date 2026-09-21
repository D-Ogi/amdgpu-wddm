#!/usr/bin/env python3
"""Ask amdgpu what a user-mode driver asks it: every AMDGPU_INFO query of info.json (generated from the UAPI header by
gen_info.py), raw. Read-only: the INFO ioctl changes nothing. Output: one line per query with the reply as hex words,
trailing zero words cut. Decoding is left to the PC side, which has the header.

    python3 -u info.py info.json

struct drm_amdgpu_info is 32 bytes: return_pointer (u64), return_size (u32), query (u32), then a 16-byte union whose
first words are (type, ip_instance) for HW_IP_*, (fw_type, ip_instance, index) for FW_VERSION and (type) for SENSOR and
VIDEO_CAPS.
"""

import ctypes
import fcntl
import glob
import json
import os
import struct
import sys

REPLY = 2048


def iow(kind, number, size):
    return (1 << 30) | (size << 16) | (ord(kind) << 8) | number


def ask(fd, request, query, a=0, b=0, c=0):
    reply = ctypes.create_string_buffer(REPLY)
    arg = struct.pack("<QII4I", ctypes.addressof(reply), REPLY, query, a, b, c, 0)
    try:
        fcntl.ioctl(fd, request, arg)
    except OSError as e:
        return f"ERROR {e.errno}"
    words = list(struct.unpack(f"<{REPLY // 4}I", reply.raw))
    while len(words) > 1 and words[-1] == 0:
        words.pop()
    return f"{len(words)} words: " + " ".join(f"{w:08x}" for w in words)


def main():
    info = json.load(open(sys.argv[1]))
    nodes = sorted(glob.glob("/dev/dri/renderD*"))
    if not nodes:
        sys.exit("no render node: amdgpu not loaded")
    fd = os.open(nodes[0], os.O_RDWR)
    request = iow("d", 0x40 + info["nr"], 32)       # DRM_COMMAND_BASE is 0x40 (drm.h), DRM_IOW
    print(f"# {nodes[0]} request 0x{request:08x}")
    q = info["queries"]
    plain = [k for k in q if k not in ("AMDGPU_INFO_HW_IP_INFO", "AMDGPU_INFO_HW_IP_COUNT", "AMDGPU_INFO_FW_VERSION",
                                       "AMDGPU_INFO_SENSOR", "AMDGPU_INFO_VIDEO_CAPS")]
    for name in plain:
        print(f"{name}: {ask(fd, request, q[name])}")
    for ip, value in info["hw_ip"].items():
        if ip.endswith("_MAX_COUNT"):
            continue
        print(f"AMDGPU_INFO_HW_IP_COUNT {ip}: {ask(fd, request, q['AMDGPU_INFO_HW_IP_COUNT'], value)}")
        for instance in (0, 1):
            print(f"AMDGPU_INFO_HW_IP_INFO {ip} instance {instance}: {ask(fd, request, q['AMDGPU_INFO_HW_IP_INFO'], value, instance)}")
    for fw, value in info["fw"].items():
        for index in (0, 1):
            print(f"AMDGPU_INFO_FW_VERSION {fw} index {index}: {ask(fd, request, q['AMDGPU_INFO_FW_VERSION'], value, 0, index)}")
    for sensor, value in info["sensor"].items():
        print(f"AMDGPU_INFO_SENSOR {sensor}: {ask(fd, request, q['AMDGPU_INFO_SENSOR'], value)}")
    for cap, value in info["video_caps"].items():
        print(f"AMDGPU_INFO_VIDEO_CAPS {cap}: {ask(fd, request, q['AMDGPU_INFO_VIDEO_CAPS'], value)}")
    os.close(fd)


if __name__ == "__main__":
    main()
