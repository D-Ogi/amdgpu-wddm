#!/usr/bin/env python3
"""Generate info.json for info.py from the amdgpu UAPI header: the AMDGPU_INFO query ids, the sub-queries of FW_VERSION,
SENSOR, VRAM_GTT-like queries and the hardware IP types. No number is typed here; the header is Mesa's copy of the
kernel's include/uapi/drm/amdgpu_drm.h (P:/BC-250/ref/mesa/include/drm-uapi/amdgpu_drm.h).

Run:  python experiments/E13-linux-reference-2/gen_info.py [header]
"""

import json
import re
import sys
from pathlib import Path

HEADER = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"P:\BC-250\ref\mesa\include\drm-uapi\amdgpu_drm.h")
DEFINE = re.compile(r"^#define\s+(AMDGPU_(?:INFO|HW_IP)_\w+|DRM_AMDGPU_INFO)\s+(0x[0-9a-fA-F]+|\d+)\s*(?:/\*.*)?$")

# Left out on purpose: the VBIOS image and its strings stay out of the repo, register reads go through regs2.py's named
# list, and CRTC_FROM_ID / NUM_HANDLES say nothing about the GPU.
SKIP = {"AMDGPU_INFO_VBIOS", "AMDGPU_INFO_READ_MMR_REG", "AMDGPU_INFO_CRTC_FROM_ID", "AMDGPU_INFO_NUM_HANDLES",
        "AMDGPU_INFO_MMR_SE_INDEX_SHIFT", "AMDGPU_INFO_MMR_SE_INDEX_MASK", "AMDGPU_INFO_MMR_SH_INDEX_SHIFT",
        "AMDGPU_INFO_MMR_SH_INDEX_MASK"}


def main():
    names = {}
    for line in HEADER.read_text(encoding="utf-8", errors="replace").splitlines():
        m = DEFINE.match(line.strip())
        if m and m.group(1) not in SKIP:
            names[m.group(1)] = int(m.group(2), 0)
    fw = {k: v for k, v in names.items() if k.startswith("AMDGPU_INFO_FW_") and k != "AMDGPU_INFO_FW_VERSION"}
    sensor = {k: v for k, v in names.items() if k.startswith("AMDGPU_INFO_SENSOR_")}
    ras = {k for k in names if k.startswith("AMDGPU_INFO_RAS_")}
    video = {k: v for k, v in names.items() if k.startswith("AMDGPU_INFO_VIDEO_CAPS_")}
    ip = {k: v for k, v in names.items() if k.startswith("AMDGPU_HW_IP_") and k != "AMDGPU_HW_IP_NUM"}
    top = {k: v for k, v in names.items() if k.startswith("AMDGPU_INFO_") and k not in fw and k not in sensor and k not in ras
           and k not in video and not k.startswith("AMDGPU_INFO_VBIOS_")}
    out = {"header": str(HEADER).replace("\\", "/"), "nr": names["DRM_AMDGPU_INFO"], "queries": top, "fw": fw, "sensor": sensor,
           "video_caps": video, "hw_ip": ip}
    target = Path(__file__).resolve().parent / "info.json"
    target.write_text(json.dumps(out, indent=1), encoding="utf-8", newline="\n")
    print(f"{len(top)} queries, {len(fw)} firmware types, {len(sensor)} sensors, {len(ip)} IP types -> {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
