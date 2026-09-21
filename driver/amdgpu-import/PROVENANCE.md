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
| `v10_structs.h` | `drivers/gpu/drm/amd/include/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `nvd.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `clearstate_defs.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2012 Advanced Micro Devices, Inc. |
| `clearstate_gfx10.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `amdgpu_doorbell.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2018 Advanced Micro Devices, Inc. |
| `sdma_common.h` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `reference/gfx_v10_0.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |
| `reference/sdma_v5_0.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2019 Advanced Micro Devices, Inc. |

Every one of the files carries the full MIT permission notice; only `cyan_skillfish_reg_init.c` also
carries the SPDX tag. Checked file by file, not assumed from the directory.

The commit column is the same for every row because `P:\BC-250\ref\linux-src` is a depth-1 clone of
the tag, so the tag commit is the only one it has. It is the commit the bytes come from, which is
what the column is for; it is not the last commit that touched each file, and `git log -- <path>`
in that clone cannot tell us what that was.

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

## GFX, KIQ and SDMA (milestone M5 part B)

Six headers are compiled: `v10_structs.h` (the MQD layouts), `nvd.h` (the GFX10 PM4 packet
definitions), `clearstate_defs.h` and `clearstate_gfx10.h` (the clear-state section tables and the
921 register values in them), `amdgpu_doorbell.h` (the NAVI10 doorbell index assignment) and
`sdma_common.h` (the two UTCL2 cache-policy enums the SDMA `UTCL1_PAGE` write uses). All six have no
`#include` lines of their own; the shim supplies the handful of typedefs they reach for. Note that `clearstate_gfx10.h` defines `static const` arrays, so it is included in exactly one
translation unit (`driver/shim/bc250_gfx.c`), and that `soc15d.h` is deliberately **not** imported:
it defines `PACKET3` a second time and its include guard is `SOC15_H`, the same as `soc15.h`.

`reference/gfx_v10_0.c` and `reference/sdma_v5_0.c` are **reference only: they are never compiled**.
That is why they sit in a subdirectory: `driver/kmd/build.ps1` globs `driver/amdgpu-import/*.c`
non-recursively, so everything directly in this directory compiles and everything under `reference/`
cannot be picked up by accident. They are here for
two reasons. First, every function in `driver/shim/bc250_gfx.c` and `bc250_sdma.c` carries a comment
naming the upstream function and line it follows, and a reviewer can check that citation against a
file in this tree instead of against a kernel checkout at the right tag. Second, the one data table
we need out of `gfx_v10_0.c`, `golden_settings_gc_10_0_cyan_skillfish`, is extracted from this copy
mechanically by `tools/import/extract_table.py` into
`driver/shim/generated/gfx10_golden_cyan_skillfish.inc`, and the same is done with
`golden_settings_sdma_cyan_skillfish` out of `sdma_v5_0.c` into
`driver/shim/generated/sdma5_golden_cyan_skillfish.inc`; the replay test re-runs both extractions
and diffs, so no register address, mask or value is ever typed by hand and an upstream change to a
table fails loudly instead of silently.

They are not compiled because they cannot be: `gfx_v10_0.c` is 10247 lines and calls about ninety
functions it does not define, among them the buffer-object allocator, the writeback allocator,
`amdgpu_ring_init`, the DRM scheduler, `dma_fence`, `request_firmware` and the ucode header parsers,
interrupt registration, sysfs, debugfs and delayed work. A shim for that would be a partial Linux.
`sdma_v5_0.c` is the same story at 2102 lines and thirty external functions, of which only three are
reachable from the ring bring-up path. What those two files do for the hardware is reimplemented in
`driver/shim/bc250_gfx.c` and `bc250_sdma.c`, function by function, with the citation comments.

### Deviations from the imported tag

Where our transcription does not follow mainline v6.18, it is because the trace decides. Unit A ran
Alpine's `6.18.52-0-lts`, not the tag the imports come from, and where the two disagree the kernel
that produced the trace is the one that is right.

| Where | Mainline v6.18 | 6.18.52-0-lts, and what unit A shows | Our choice |
| --- | --- | --- | --- |
| `gfx_v10_0_cp_gfx_resume()`, `CP_RB_DOORBELL_CONTROL` / `CP_RB0_WPTR_POLL_CNTL` | `gfx_v10_0.c:5348` region has no `DB_RING_CONTROL` write | `gfx_v10_0.c:5352` writes it; the E03 trace contains the write | follow 6.18.52, comment at the write site in `driver/shim/bc250_gfx.c` |
| `amdgpu_gmc_noretry_set()` | tests `gc_ver >= IP_VERSION(10, 3, 0)`, giving `noretry = false` for GC 10.1.3 | tests `>= IP_VERSION(10, 1, 0)`, giving `true`; the traced `GCVM_CONTEXT1..15_CNTL` have the retry bit clear | `noretry = true`, measured; see `driver/shim/README.md` |

Both are recorded rather than silently absorbed, because a future reader comparing our code with a
mainline checkout would otherwise find a difference and have no way to tell whether it was a bug.

### Deviations from upstream's behaviour, where upstream has no mechanism for this part

The two above follow a different kernel. This one follows neither, and is the only place where this
driver knowingly does something amdgpu does not.

| Where | What upstream does | Why it cannot work here | What we do instead |
| --- | --- | --- | --- |
| `gfx_v10_0_kiq_init_register()`, the `CP_HQD_ACTIVE` branch (`gfx_v10_0.c:7034-7050`), transcribed in `driver/shim/bc250_gfx.c` | Writes `CP_HQD_DEQUEUE_REQUEST = 1` and polls `CP_HQD_ACTIVE` until the CP clears it (`:7036-7041`) | A dequeue request is serviced by the MEC, and on a re-init without a GPU reset the MEC is halted: `hw_fini` halted it and `gfx_v10_0_cp_compute_enable(adev, true)` runs at `:7208` from `kcq_resume` (`:7243`), one step **after** `kiq_resume` (`:7239`). Nothing else clears the KIQ's own `CP_HQD_ACTIVE` - `hw_fini` unmaps the client queues but never the KIQ (`amdgpu_gfx.c:531, :584`), and the only `CP_HQD_ACTIVE = 0` in the file is SR-IOV's at `:7028-7029`. Upstream never notices because it does not check the poll's outcome (`j` unused after `:7041`) and its parts drop GFX power across suspend or take a reset, so the branch is not entered | Read `CP_MEC_CNTL`; if a halt bit is set, take upstream's own `:7029` write (`CP_HQD_ACTIVE = 0`) on that measured condition instead of the handshake, and skip only the `CP_HQD_DEQUEUE_REQUEST` restore, which exists to undo a request that was never made. The live-CP arm is upstream's, unchanged. Cold boot is unaffected: `CP_HQD_ACTIVE` reads 0 and the branch is not entered, which the 354-write replay confirms |

Measured consequence, from `driver/shim/test/run_gfx.ps1`: with this, teardown followed by the whole
bring-up again succeeds (357 writes against the first run's 354, 13 ring tests across the two), and
the second run touches **no register outside the trace windows the miniport's table is generated
from**. The 3-write difference is fully accounted for: +4 from this branch (`CP_HQD_ACTIVE`,
`CP_HQD_PQ_RPTR`, `CP_HQD_PQ_WPTR_LO`, `CP_HQD_PQ_WPTR_HI`) and -1 for `RLC_SPM_MC_CNTL`, which
upstream's own `pre_data != data` guard (`gfx_v10_0.c:8297`) skips because the field already holds
the value the first run put there.
