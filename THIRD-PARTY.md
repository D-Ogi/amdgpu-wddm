# Third-party material

Our own code and documentation: PolyForm Noncommercial 1.0.0, licensor D-Ogi (`LICENSE.md`, `NOTICE`). Everything listed below is NOT ours and keeps its own license.

## In this repository

| Path | Origin | License | Notes |
|---|---|---|---|
| `third_party/linux-amdgpu/*.h` | Linux kernel, `drivers/gpu/drm/amd/include/` (per-file commit in `PROVENANCE.md` there) | MIT (notice in each file), Copyright Advanced Micro Devices, Inc. | Unmodified register headers: offsets, field masks and shifts, reset defaults, the memory-type enum, hardware IP ids. Input of `tools/regcalc` and of the imports below |
| `driver/amdgpu-import/*.c`, `*.h` | Linux kernel, `drivers/gpu/drm/amd/amdgpu/`, tag v6.18, commit `7d0a66e4bb90` (see `PROVENANCE.md` there) | MIT (notice checked in each file), Copyright Advanced Micro Devices, Inc. | Unmodified IP-block sources (ADR 0002): the GFXHUB and MMHUB GART setup, the Cyan Skillfish register base table, the SOC15 register macros. Compiled against `driver/shim`, never edited |

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
| Further Linux `amdgpu` IP-block sources for the shim-based import, ADR 0002 (`gmc_v10_0.c`, `gfx_v10_0.c`, `psp_v11_0_8.c`, `smu_v11_0`, `navi10_ih.c`, `sdma_v5_0.c`, `nv.c`) | MIT per file, check each | Record the kernel commit next to each file, as `driver/amdgpu-import/PROVENANCE.md` does for the first batch |
