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
| `reference/navi10_ih.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2018 Advanced Micro Devices, Inc. |
| `reference/amdgpu_ih.c` | `drivers/gpu/drm/amd/amdgpu/` | v6.18 | `7d0a66e4bb90` | MIT text, Copyright 2014 Advanced Micro Devices, Inc. |

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

The two above follow a different kernel. These follow neither, and are the only places where this
driver knowingly does something amdgpu does not. Both are in the same branch, and that is not a
coincidence: it is the branch only a re-initialisation without a GPU reset enters, and this part is
the one that gets there because it keeps GFX powered across a teardown.

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

#### The read pointer a re-initialised queue is given

| Where | What upstream does | Why it cannot work here | What we do instead |
| --- | --- | --- | --- |
| `gfx_v10_0_compute_mqd_init()` (`gfx_v10_0.c:6998`), transcribed in `driver/shim/bc250_gfx.c` | `mqd->cp_hqd_pq_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR)` - samples the register, under a GRBM selection of this queue's own slot, whatever the comment above it ("reset read and write pointers") says | The sample is clean only on a part that loses GFX power between loads. This one does not, so on a second bring-up the slot still holds the first run's read pointer. `gfx_v10_0_kiq_init_register()` then writes it back at `:7044` - in the `CP_HQD_ACTIVE` branch above, which only a re-init enters - and the CP is told there is work pending on a ring that restarts at write pointer 0. Upstream never notices: on its parts the register reads 0, and it took the same fresh-init arm (`:7151`) on first load and on resume alike, since the reset arm at `:7137` needs `amdgpu_in_reset()` | `mqd->cp_hqd_pq_rptr = 0`. It is the only value consistent with the rest of the same function, which sets `cp_hqd_pq_wptr_lo` and `_hi` to 0 four lines earlier, and with `amdgpu_ring_init_mqd()`'s `ring->wptr = 0` (`amdgpu_ring.c:743`). It is also what AMD write from the next generation on: `gfx_v11_0.c:4337` and `gfx_v12_0.c:3216` use `regCP_HQD_PQ_RPTR_DEFAULT`, defined as 0 in `gc_11_0_0_default.h:2091` |

Measured on unit A, experiment E12 run 001: the second bring-up in one boot wrote
`CP_HQD_PQ_RPTR = 0x737`, and stage 6 (CP resume) took **5222 us against 349 us** on the cold run
and in every E11 run. The KIQ ring is 1 MB - 262144 dwords, confirmed independently by the traced
`CP_HQD_PQ_CONTROL = 0xD0300911`, whose `QUEUE_SIZE` field is 17 and whose size is therefore
`2 << 17` - so a read pointer of 0x737 = 1847 against a write pointer of 0 presents
262144 - 1847 = **260297 dwords** of apparent work. 4873 us of extra time over 260297 dwords is
53.4 dwords/us, which accounts for the delay on its own; the eight compute rings, 2048 dwords each,
can add at most ~300 us. The same run's `CP_HQD_PQ_CONTROL` bit 15 is `PQ_EMPTY`
(`gc_10_1_0_sh_mask.h:20323`), set on the cold run and clear on the second: the hardware saying in a
second register that it does not consider the queue empty.

**Falsifiable prediction.** With this change, stage 6 of a second bring-up in one boot returns to
about 350 us - the same as a cold run - because the CP is handed a read pointer equal to its write
pointer and fetches nothing until the doorbell. If a rebuilt driver still takes milliseconds there,
this explanation is wrong and the row should come out.

Cold boot is unaffected by construction: the register reads 0 there, so the sampled value was
already 0. That is checked and not argued - the first run's nine MQDs, dumped field by field
(`<out>/dumps/kiq-mqd.txt`, 19381 bytes), are byte-identical before and after the change, and the
354 + 35 comparison against unit A is unmoved.

`check_rerun()` in `driver/shim/test/replay_gfx.c` carries the test, and it needs one declared model
to do so: the host replay has no CP to advance a read pointer, so nothing would ever put a non-zero
value in `CP_HQD_PQ_RPTR` and the second run would look clean. The measured 0x737 is poked into the
register before the re-run, from that one hardware run, and what the shim writes back is checked.
That gap is why this reached hardware before it reached the host test: the host test found the
branch, unit A found the value.

## Interrupts (milestone M6)

Nothing new is compiled. `reference/navi10_ih.c` and `reference/amdgpu_ih.c` are **reference only**,
under `reference/` for the same build-glob reason as the two above, and `driver/shim/bc250_ih.c`
follows them function by function with a citation at each one.

They are transcribed rather than compiled for the usual reason and one more. `navi10_ih.c` reaches
into the IP-block machinery, the buffer-object allocator (`amdgpu_ih_ring_init()` is
`amdgpu_bo_create_kernel` plus the write-back pool) and the Linux interrupt subsystem
(`amdgpu_irq_init`, `pci_set_master`, the irq domain), none of which exists here. The extra reason
is that three of the functions have to run in the miniport's DPC at `DISPATCH_LEVEL`, where no lock
may be taken, nothing may be allocated and nothing may sleep; a transcription is what makes it
possible to say that of each line. `driver/shim/include/bc250_ih.h` lists, with a citation each,
every arm of `navi10_ih.c` that is deliberately not transcribed - ih1 and ih2, `ih_soft`, the
`IH_CHICKEN` block, `force_update_wptr_for_self_int()`, every `amdgpu_sriov_vf()` arm,
`IH_STORM_CLIENT_LIST_CNTL` and `pci_set_master()`.

Only the decode comes from `amdgpu_ih.c`: `amdgpu_ih_decode_iv_helper()` at `:263`, which is the one
statement in the kernel of what the 32 bytes of an interrupt vector mean. The rest of that file is
the ring allocator and the submission loop, and both belong to Linux.

Two things that look like local decisions are upstream's and are taken over as such, because both
turn out to be what makes a Windows DPC possible:

- `struct amdgpu_ih_regs` (`amdgpu_ih.h:29`), the register offsets carried on the ring and filled
  once by `navi10_ih_init_register_offset()` (`navi10_ih.c:49`). `navi10_ih.c` reads them back out
  of `ih->ih_regs` everywhere, `navi10_ih_set_rptr()` included. Following it means the DPC-safe
  functions never call `SOC15_REG_OFFSET()`, which resolves through `adev->reg_offset[][]` - a table
  the miniport's DPC-only `struct amdgpu_device` has no reason to carry, and a null dereference at
  `DISPATCH_LEVEL` if it did not. Upstream's `psp_reg_id` member is dropped: its only reader is the
  SR-IOV `psp_reg_program()` path.
- `WDOORBELL32` in `navi10_ih_set_rptr()` (`navi10_ih.c:474`, `:499`). Every other doorbell on this
  part is 64-bit (`gfx_v10_0.c:8571`, `:8605`, `sdma_v5_0.c:389`); this one is not, and the
  difference reaches the hardware, because `BIF_IH_DOORBELL_RANGE` opens a two-entry window and the
  upper half of a 64-bit store lands inside it. `bc250_shim_wdoorbell32()` exists for this one
  caller, and the host backend records the width of every doorbell so the test can insist on it.

The fence emitters are in `reference/gfx_v10_0.c`, not in these two:
`gfx_v10_0_ring_emit_fence()` at `:8712` and `gfx_v10_0_ring_emit_fence_kiq()` at `:8780`,
transcribed together as `bc250_gfx_emit_fence()` in `driver/shim/bc250_gfx.c`. Upstream picks
between them through `ring->funcs->emit_fence`; the shim's `amdgpu_ring_funcs` carries no callbacks
(ADR 0002), so the one function branches on `ring->funcs->type`. The two `BUG_ON()`s upstream uses
for a misaligned address and for a 64-bit KIQ fence become `BC250_EINVAL` with nothing written: the
conditions are upstream's, and only the reaction differs, because a `BUG()` in a Windows miniport
is a bugcheck on a caller's mistake.

### What the M6 host test can and cannot say

`driver/shim/test/run_ih.ps1` replays the fifteen register writes of `navi10_ih_irq_init()` against
the trace window at 0.252832-0.252845 and matches them exactly, with four address exceptions
(`IH_RB_BASE`, `IH_RB_BASE_HI`, `IH_RB_WPTR_ADDR_LO` and `_HI`, each separately checked against the
allocation it describes). That part is measured.

The decode, the write- and read-pointer functions and the fence are exercised against vectors the
test builds and against vectors the CP stub in `driver/shim/test/backend_mem.c` delivers. No
interrupt is raised by hardware anywhere in this, and the stub's encoder is the test's own code.
What the test establishes is therefore that the encode and the decode agree, and that the packet
the shim emits is the packet upstream's emitter builds - not that this ASIC raises the interrupt.
That is the first hardware run's job, and it is what the fence exists for.

One model is declared, through `backend_add_selfclear()`: `IH_RB_CNTL` bit 31,
`WPTR_OVERFLOW_CLEAR`, does not stay where it is written. The trace says so outright - `C03101A0`
goes in at 0.252838 and `403101A0` comes back at 0.252845 - and the last read-modify-write of the
sequence depends on it. The write is still compared exactly as the driver issued it; only the value
a later read returns is corrected. A control run removes the declaration and fails, which is what
says the declaration is doing work rather than covering a difference.
