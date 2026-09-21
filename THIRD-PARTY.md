# Third-party material

Our own code and documentation: PolyForm Noncommercial 1.0.0, licensor D-Ogi (`LICENSE.md`, `NOTICE`). Everything listed below is NOT ours and keeps its own license.

## In this repository

| Path | Origin | License | Notes |
|---|---|---|---|
| `third_party/linux-amdgpu/*.h` | Linux kernel, `drivers/gpu/drm/amd/include/` (see `PROVENANCE.md` there) | MIT (notice in each file), Copyright Advanced Micro Devices, Inc. | Unmodified register offset headers. Input of `tools/regcalc` |

## Fetched at build time, not stored here

| What | Used by | License |
|---|---|---|
| Alpine Linux 3.24 "standard" ISO and packages (`python3`, `pciutils` and dependencies) | `tools/diagusb/build_usb.py` | Various free software licenses, see alpinelinux.org. Includes the Linux kernel (GPL-2.0) and `linux-firmware` blobs under their vendors' redistribution licenses |
| `segno` (pure-Python QR encoder) | `tools/diagusb` payload | BSD-3-Clause |

## Referenced, never copied

| What | Why not |
|---|---|
| AMD GPU firmware (`cyan_skillfish2_*.bin`) | AMD redistributable-binary license. The user obtains it from `linux-firmware` |
| `ZEROAESQUERDA/BC250-windowsDriverTest` | No license granted |

## Candidates for borrowing later (not yet imported)

| What | License | Condition |
|---|---|---|
| `Keshas-dev/AMD-BC-250-Windows-Driver`: SMU mailbox, PSP GPCOM ring | Apache-2.0 | Attribute here, keep the notice, re-verify under `docs/01-evidence-rules.md` |
| `Keshas-dev/AMD-BC-250-PSP-Driver`: BIOS/PSP directory parsing scripts | MIT | same |
| Linux `amdgpu` IP-block sources for the shim-based import (ADR 0002) | MIT per file, check each | Record the kernel commit next to each file |
