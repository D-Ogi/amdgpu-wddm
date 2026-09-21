# Provenance

Unmodified copies from the Linux kernel, tag **v6.18**, commit
`7d0a66e4bb9081d75c82ec4957c50034cb0ea449`, taken 2026-09-21 from a local checkout
(`P:\BC-250\ref\linux-src`, see `linux-src.PROVENANCE.txt` next to it).

Taken with `git show`, so the bytes are the kernel's own, with LF line endings and no checkout
normalization:

```
git -C <linux checkout> show v6.18:drivers/gpu/drm/amd/amdgpu/<file> > driver/amdgpu-import/<file>
```

| File | Path in the kernel tree | Tag | Commit | License notice in the file |
|---|---|---|---|---|
| `gfxhub_v2_0.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `gfxhub_v2_0.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `mmhub_v2_0.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `mmhub_v2_0.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `cyan_skillfish_reg_init.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | `SPDX-License-Identifier: MIT` plus MIT text, Copyright 2018 Advanced Micro Devices, Inc. |
| `soc15_common.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2016 Advanced Micro Devices, Inc. |
| `psp_v11_0_8.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2021 Advanced Micro Devices, Inc. |
| `psp_v11_0_8.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2021 Advanced Micro Devices, Inc. |
| `psp_gfx_if.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2017 Advanced Micro Devices, Inc. |

Every one of the files carries the full MIT permission notice; only `cyan_skillfish_reg_init.c` also
carries the SPDX tag. Checked file by file, not assumed from the directory.

`soc15_common.h` compiles under the shim as it is, so it is imported rather than reimplemented: the
shim supplies the names its macros reach for (`RREG32`/`WREG32`, `amdgpu_sriov_vf` and the two
`amdgpu_sriov_*reg` accessors, `REG_FIELD_MASK`/`REG_FIELD_SHIFT`, `AMDGPU_REGS_RLC`,
`AMDGPU_REGS_NO_KIQ`, and the `adev->gfx.rlc` fields its RLC test reads).

The register headers these files include live in `third_party/linux-amdgpu/` with their own
`PROVENANCE.md`. They are included as `"gc/gc_10_1_0_offset.h"` and friends; the shim answers that
with a one-line forwarding header in `driver/shim/include/gc/` and `driver/shim/include/mmhub/`,
so that neither the imports nor the headers have to be touched.

## PSP (milestone M5)

`psp_v11_0_8.c` is the whole register side of the PSP on this chip: ring create, ring stop, write
pointer. It names `psp_wait_for`, `mdelay`, `adev->firmware.rbuf` and `amdgpu_bo_free_kernel`; the
shim supplies them (`driver/shim/include/amdgpu_psp.h`, `amdgpu.h`, `bc250_psp.c`). `psp_gfx_if.h`
is AMD's interface header for the ring frames and command buffers, used as it is; its nameless
union and `uint32_t` bit fields raise C4201 and C4214 at `/W4`, and `psp_v11_0_8_ring_stop()` does
not use its `ring_type` argument (C4100): all three are switched off from the build line
(`driver/shim/test/run_psp.ps1`, `driver/kmd/build.ps1`), nowhere else. `amdgpu_psp.c` itself is
not imported: see `driver/shim/include/bc250_psp.h` for why and for what follows it instead.
