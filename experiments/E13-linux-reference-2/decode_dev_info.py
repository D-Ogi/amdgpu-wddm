#!/usr/bin/env python3
"""Decode the AMDGPU_INFO_DEV_INFO reply of info.txt by the structure in the UAPI header, so that nobody counts words
by hand. The member list of struct drm_amdgpu_info_device is parsed from amdgpu_drm.h, nothing is typed here.

Run:  python decode_dev_info.py <info.txt> [<amdgpu_drm.h>]
"""

import os
import re
import sys
from pathlib import Path

# BC250_ROOT is the workspace root; by default the parent directory of this repository.
ROOT = Path(os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[2].parent)))
HEADER = ROOT / "ref" / "mesa" / "include" / "drm-uapi" / "amdgpu_drm.h"
SIZES = {"__u32": 4, "__u64": 8, "__s32": 4, "__u16": 2, "__u8": 1}


def members(header):
    text = re.sub(r"/\*.*?\*/", "", header.read_text(encoding="utf-8"), flags=re.S)
    body = re.search(r"struct drm_amdgpu_info_device\s*\{(.*?)\n\};", text, flags=re.S).group(1)
    out = []
    for kind, name, dims in re.findall(r"(__\w+)\s+(\w+)((?:\[\w+\])*)\s*;", body):
        count = 1
        for d in re.findall(r"\[(\w+)\]", dims):
            count *= int(d, 0) if d[0].isdigit() else int(re.search(rf"#define\s+{d}\s+(\w+)", text).group(1), 0)
        out.append((kind, name, count))
    return out


def main():
    info = Path(sys.argv[1])
    header = Path(sys.argv[2]) if len(sys.argv) > 2 else HEADER
    line = next(l for l in info.read_text(encoding="utf-8").splitlines() if l.startswith("AMDGPU_INFO_DEV_INFO:"))
    words = [int(w, 16) for w in line.split("words:")[1].split()]
    data = b"".join(w.to_bytes(4, "little") for w in words)
    printed = len(data)
    data += bytes(4096)             # info.py cuts trailing zero words: what follows the last printed word is zero
    offset = 0
    for kind, name, count in members(header):
        size = SIZES[kind]
        offset = (offset + size - 1) // size * size        # natural alignment, as the kernel lays it out
        values = [int.from_bytes(data[offset + i * size:offset + (i + 1) * size], "little") for i in range(count)]
        shown = " ".join(f"0x{v:x}" for v in values)
        if count == 1:
            shown += f"  ({values[0]})"
        print(f"+{offset:4d} {kind} {name}{'[' + str(count) + ']' if count > 1 else ''} = {shown}")
        offset += size * count
    print(f"structure: {offset} bytes; printed by info.py: {printed} bytes, the rest was zero")


if __name__ == "__main__":
    main()
