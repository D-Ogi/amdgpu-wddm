# Third-party material

Our own code and documentation: PolyForm Noncommercial 1.0.0, licensor D-Ogi (`LICENSE.md`, `NOTICE`). Everything listed below is NOT ours and keeps its own license.

## In this repository

| Path | Origin | License | Notes |
|---|---|---|---|
| `third_party/linux-amdgpu/*.h` | Linux kernel, `drivers/gpu/drm/amd/include/` (per-file commit in `PROVENANCE.md` there) | MIT (notice in each file), Copyright Advanced Micro Devices, Inc. | Unmodified register headers: offsets, field masks and shifts, reset defaults, the memory-type enum, hardware IP ids. Input of `tools/regcalc` and of the imports below |
| `driver/amdgpu-import/*.c`, `*.h` | Linux kernel, `drivers/gpu/drm/amd/amdgpu/`, tag v6.18, commit `7d0a66e4bb90` (see `PROVENANCE.md` there) | MIT (notice checked in each file), Copyright Advanced Micro Devices, Inc. | Unmodified IP-block sources (ADR 0002): the GFXHUB and MMHUB GART setup, the Cyan Skillfish register base table, the SOC15 register macros, the PSP 11.0.8 ring code and the PSP interface header. Compiled against `driver/shim`, never edited |
| `driver/amdgpu-import/reference/{dc_resource.c,fixed31_32.h,fixpt31_32.c}`, `driver/shim/generated/dc_scaler_geometry.inc` | Linux amdgpu DC, v6.18, commit `7d0a66e4bb90` | MIT, Copyright 2012-15 Advanced Micro Devices, Inc. | Verbatim scaler geometry and fixed-point functions. The separate checked Windows adapter uses the project license |
| `driver/amdgpu-import/reference/{dcn20_dpp.c,dcn10_dpp_dscl.c,dc_spl_types.h}`, `driver/shim/generated/dc_scaler_buffer.inc` | Linux amdgpu DC, v6.18, commit `7d0a66e4bb90` | MIT, Copyright Advanced Micro Devices, Inc. | Verbatim line-buffer accounting and configuration enum. The separate checked Windows adapter uses the project license |

`driver/shim/generated/dc_scaler_filters.inc` retains the AMD MIT tables,
selectors and fixed-point helpers from Linux amdgpu DC v6.18 `7d0a66e4bb90`.
Its notices are included. The separate filter-packing adapter and linear
weight generation use the project license.

`driver/amdgpu-import/reference/dcn201_dpp.c` and the extracted tap-selection
block in `driver/shim/generated/dc_scaler_taps.inc` retain AMD MIT notices.
The latter also contains the AMD MIT fixed-point ceiling function. Both are
from Linux amdgpu DC v6.18 `7d0a66e4bb90`; the combined planner is project code.

`driver/amdgpu-import/reference/{dcn201_mpc.c,dcn20_mpc.c,mpc.h}` and
`driver/shim/generated/dc_mpc_blend.inc` retain the AMD MIT notices.
The extracted DCN201 defaults, blend packing and enums are unchanged except
for line endings. The checked two-plane transaction adapter uses the project
license. Source revision: Linux amdgpu DC v6.18 `7d0a66e4bb90`.

`driver/amdgpu-import/reference/dcn10_hubp.c` and the existing
`reference/hdmi-link/dc_hw_types.h` supply `driver/shim/generated/dc_surface_layout.inc`.
These AMD MIT sources are Linux v6.18 `7d0a66e4bb90`. The extractor retains
the declarations and functions with LF endings, and adds private-linkage
prototypes. The checked Windows layout adapter uses the project license.

`driver/shim/generated/dc_cnvc_color.inc` retains the AMD MIT notices,
CNVC setup, declarations and input CSC table from Linux v6.18 `7d0a66e4bb90`.
Its reference inputs are `dcn201_dpp.c`, `dpp.h`, `hw_shared.h`, `dcn10_dpp.h`
and `hdmi-link/dc_hw_types.h`. The separate CPU capture adapter defers CSC
bank selection and power operations, and explicitly disables XRGB alpha.

## Fetched at build time, not stored here

| What | Used by | License |
|---|---|---|
| Alpine Linux 3.24 "standard" ISO and packages (`python3`, `pciutils` and dependencies) | `tools/diagusb/build_usb.py` | Various free software licenses, see alpinelinux.org. Includes the Linux kernel (GPL-2.0) and `linux-firmware` blobs under their vendors' redistribution licenses |
| `segno` (pure-Python QR encoder) | `tools/diagusb` payload | BSD-3-Clause |
| AMD GPU Services (AGS) SDK 6.3.0: `amd_ags.h`, `amd_ags_x64.dll`, `amd_ags_x86.dll` (GPUOpen-LibrariesAndSDKs/AGS_SDK, tag v6.3.0) | `tools/build/build-umd-adl.ps1` (`-AgsSdk`, the header of the test client) and `tools/build/test-umd-adl-ags.ps1` (the libraries) | MIT |

## Referenced, never copied

| What | Why not |
|---|---|
| AMD GPU firmware (`cyan_skillfish2_*.bin`) | AMD redistributable-binary license. The user obtains it from `linux-firmware` |
| `ZEROAESQUERDA/BC250-windowsDriverTest` | No license granted |
| GPUOpen-LibrariesAndSDKs/display-library (ADL SDK) @ 84c881cb: `include/adl_sdk.h`, `adl_defines.h`, `adl_structures.h` | The headers are MIT, but the repository has no top-level license and its documents and samples are under an AMD license. `driver/umd/adl/adl-abi.h` declares again only the types and constants that the library uses, from the three MIT headers (PROVENANCE line there). No sample code is used |
| `bc250-encoding-decoding-fix` (GPL-3.0-only): read for its per-stage profile of a compute H.264 encoder on this silicon, while writing `driver/umd/mft-h264` | GPL-3.0-only, which our license cannot take. No code, no shader and no table of it is here. The shared tables are the published values of ITU-T Rec. H.264 clauses 9.1.2, 9.2 and 8-16/8-17, entered from the standard. Recorded in `driver/umd/mft-h264/PROVENANCE.md` |

## Candidates for borrowing later (not yet imported)

| What | License | Condition |
|---|---|---|
| `Keshas-dev/AMD-BC-250-Windows-Driver`: SMU mailbox, PSP GPCOM ring | Apache-2.0 | Attribute here, keep the notice, re-verify under `docs/01-evidence-rules.md` |
| `Keshas-dev/AMD-BC-250-PSP-Driver`: BIOS/PSP directory parsing scripts | MIT | same |
| Further Linux `amdgpu` IP-block sources for the shim-based import, ADR 0002 (`gmc_v10_0.c`, `gfx_v10_0.c`, `psp_v11_0_8.c`, `smu_v11_0`, `navi10_ih.c`, `sdma_v5_0.c`, `nv.c`) | MIT per file, check each | Record the kernel commit next to each file, as `driver/amdgpu-import/PROVENANCE.md` does for the first batch |
