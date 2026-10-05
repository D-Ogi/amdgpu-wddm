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
driver knowingly does something amdgpu does not. They are all about one thing, which is the one
thing this part does that amdgpu's parts do not: it keeps GFX powered across a teardown and comes
back without a reset, so the second bring-up meets hardware that still remembers the first.

What that means concretely was measured rather than reasoned about, on unit A, experiment E12 run
002 (`P:\BC-250\scratch\e12\run002\out`), and it is the fact the rest of this section turns on:

**The MEC keeps its own copy of an active queue's ring base and read pointer, writes to the
`CP_HQD_*` registers while it is halted do not reach that copy, and on un-halt it resumes from it.**

The second bring-up in that run wrote `CP_HQD_ACTIVE = 0`, `CP_HQD_PQ_RPTR = 0` and
`CP_HQD_PQ_BASE = 0x5640` with `CP_MEC_CNTL = 0x50000000` - all under halt - and the engine still
fetched from the first run's ring: UTCL2 raised a VMID 0 fault at GART address `0x444000`, which is
the first run's KIQ base `0x442000` plus `0x2000`, about where that run's read pointer stood. No
register write in either run names `0x444000`; the second run's rings were at `0x564000` and
`0x563000`, because the miniport frees on FINI and gpumem does not hand a freed GART range back. The
KIQ ring test then timed out after 165 ms (`-62`) and the queue never ran again.

#### Taking the KIQ's own queue down in the teardown

| Where | What upstream does | Why it cannot work here | What we do instead |
| --- | --- | --- | --- |
| `gfx_v10_0_hw_fini()` (`gfx_v10_0.c:7529`), transcribed as `bc250_gfx_hw_fini()` in `driver/shim/bc250_gfx.c` | Unmaps the client queues through the KIQ (`amdgpu_gfx.c:501`, `:551`) and halts the CP and the MEC (`:7563`). The KIQ's own HQD is never dequeued - nothing in the file ever takes it down | The engine's copy of the KIQ's base and read pointer survives the halt, so the next bring-up meets a live fetcher whose ring has been freed. Upstream gets away with it because its parts drop GFX power across suspend or take a reset before the next `hw_init`, either of which discards the copy; and there is no documented MEC reset to lean on instead - `kgd_gfx_v10_hqd_reset()` (`amdgpu_amdkfd_gfx_v10.c:1080-1085`) returns 0 without touching anything, and `GRBM_SOFT_RESET.SOFT_RESET_CPC` exists only from gfx11 on | `bc250_kiq_dequeue()`, called between the KCQ unmap and the MEC halt, while the engine can still answer: SRBM-select me2/pipe1/queue0, `CP_HQD_DEQUEUE_REQUEST = 1` (DRAIN_PIPE), poll `CP_HQD_ACTIVE` to 0 with `adev->usec_timeout` as the bound, restore the request register, then zero `CP_HQD_PQ_DOORBELL_CONTROL`, `CP_HQD_PQ_RPTR` and `CP_HQD_PQ_WPTR_LO/HI` and deselect. The handshake is upstream's own, from `kgd_hqd_destroy()` (`amdgpu_amdkfd_gfx_v10.c:606-619`) and from the active branch of `gfx_v10_0_kiq_init_register()` (`gfx_v10_0.c:7031-7041`); only the place it is called from is new |

`CP_HQD_DEQUEUE_REQUEST` is the one register this adds that no trace window names, and the test says
so by name rather than leaving it to be noticed. Cold boot is untouched: the teardown is not part of
the traced bring-up, and the 354 + 35 comparison against unit A is unmoved.

Upstream's teardown is not known-good on this part, which is worth stating next to a row that
departs from it: Linux itself hung this machine at `modprobe -r amdgpu` on 2026-09-21 (facts M42).
Following it exactly was never the safe option here.

#### The eight compute queues' pointer registers

| Where | What upstream does | Why it cannot work here | What we do instead |
| --- | --- | --- | --- |
| `gfx_v10_0_hw_fini()` again, against `gfx_v10_0_compute_mqd_init()`'s `mqd->cp_hqd_pq_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR)` (`gfx_v10_0.c:6998`), transcribed at `driver/shim/bc250_gfx.c:765` | Samples each queue's read pointer under that queue's own selection when building its MQD, and leaves the registers alone in the teardown | The sample is only right where the teardown left a 0 there. Here it does not: after a bring-up that ran the ring tests the queues idle at `rptr == wptr != 0` (fact M40), `UNMAP_QUEUES` is not documented or measured to clear the slot's pointer registers, and there is no power cycle in between. The next bring-up would hand each of the eight queues an MQD saying most of its ring is pending - the 5222 us walk of E12 run 001, eight times over | `bc250_kcq_clear_pointers()`, called after the KCQ unmap: for each of the eight compute rings, select its slot and zero `CP_HQD_PQ_RPTR`, `CP_HQD_PQ_WPTR_LO` and `CP_HQD_PQ_WPTR_HI`. No handshake, and none is possible or needed - these queues have already been dequeued by a running CP, and `bc250_gfx_unmap_queues()` ends in a KIQ ring test, so the packets have completed before the registers are written. All three registers are inside a trace window already, so the miniport's allow-list is unchanged |

Together with the row above, this makes one sentence true for all nine queues: the read pointer an
MQD samples is a value this driver put there. The host test fails without it - with the call taken
out, all eight compute MQDs of the second bring-up come back holding 256 - which took a fifth
declared behaviour in the backend: `CP_HQD_PQ_RPTR` answered per selected queue rather than as one
shared register, and left where the CP stub left it. See `driver/shim/test/backend_mem.h`.

#### A KIQ found active with the MEC halted

| Where | What upstream does | Why it cannot work here | What we do instead |
| --- | --- | --- | --- |
| `gfx_v10_0_kiq_init_register()`, the `CP_HQD_ACTIVE` branch (`gfx_v10_0.c:7034-7050`), transcribed in `driver/shim/bc250_gfx.c` | Writes `CP_HQD_DEQUEUE_REQUEST = 1` and polls `CP_HQD_ACTIVE` until the CP clears it (`:7036-7041`), then writes the sampled read pointer back at `:7044` | A dequeue request is serviced by the MEC, and here the MEC can be halted: `gfx_v10_0_cp_compute_enable(adev, true)` runs at `:7208` from `kcq_resume` (`:7243`), one step **after** `kiq_resume` (`:7239`). A halted engine answers nothing, so the poll spins out its whole timeout and the queue stays up. Upstream never notices, because it does not look at the poll's outcome (`j` is unused after `:7041`) and its parts do not arrive here with a queue still active | Un-halt the MEC for the length of the handshake and halt it again, then zero the read pointer instead of writing back what the MQD sampled. Reaching this branch means the last instance left this queue up - 0.6.1 and everything before it did exactly that, so the first 0.6.2 start on a machine that ran 0.6.1 lands here - and the sampled pointer is that instance's, not this one's |

Un-halting is not free: the engine resumes its stale fetch the moment it starts, and if the address
is gone it faults. That fetch happens either way, though - it is what `kcq_resume`'s own un-halt did
in run 002 two steps later - so doing it here costs one fault that was already coming and buys the
one mechanism that clears the engine's copy. The fault is survivable on this part and measured to
be: run 002 took it, the machine stayed up, the vector arrived on our own IH ring, and the teardown
afterwards freed everything.

**Not taken:** `GRBM_SOFT_RESET.SOFT_RESET_CP` with the engines already halted, AMD's own sequence
from `gfx_v10_0_soft_reset()` (`gfx_v10_0.c:7660-7685`). It is the only other mechanism that could
discard the engine's state. It is not used because upstream never soft-resets the CP without
re-running `hw_init` afterwards, which with PSP-loaded microcode means a PSP load, and this part
cannot load CP firmware a second time in one boot (facts M35). If the reset clears the CP's
instruction memory, it trades a recoverable queue for a device that needs a reboot. Whether it does
is measurable - reset, then try a ring test - and until someone measures it on unit A this stays
written down rather than shipped.

**What 0.6.1 did here, and why it was wrong.** It wrote `CP_HQD_ACTIVE = 0` on the halted condition
- upstream's own SR-IOV "inactivate the queue" write (`:7028-7029`) - on the argument that a halted
MEC has no state to corrupt. The argument was exactly backwards: a halted MEC is precisely the
engine that does not see the write. Run 002 is the refutation, and it is the same run that refuted
the row below.

#### The read pointer a re-initialised queue is given (0.6.1, refuted and reverted)

`bc250_compute_mqd_init()` now keeps upstream's sample,
`mqd->cp_hqd_pq_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR)` (`gfx_v10_0.c:6998`). The row that
stood here in 0.6.1 replaced it with `mqd->cp_hqd_pq_rptr = 0`. The reasoning was:

> On unit A's second bring-up in one boot the register held `0x737`,
> `gfx_v10_0_kiq_init_register()` wrote it back, and the CP was told there were
> 262144 - 1847 = 260297 dwords of work pending against a ring that restarts at write pointer 0. It
> walked every one of them: stage 6 took **5222 us against 349 us** cold (E12 run 001). Zero is the
> only value consistent with the rest of the same function, which sets `cp_hqd_pq_wptr_lo` and `_hi`
> to 0 four lines earlier, and with `amdgpu_ring_init_mqd()`'s `ring->wptr = 0`
> (`amdgpu_ring.c:743`); it is also what AMD write from gfx11 on (`gfx_v11_0.c:4337`,
> `gfx_v12_0.c:3216`, `regCP_HQD_PQ_RPTR_DEFAULT` = 0 in `gc_11_0_0_default.h:2091`).

The arithmetic was right and the conclusion was wrong. The register value was a symptom: what the CP
resumed from was the MEC's internal copy, which no write to that register reaches while the engine
is halted. Forcing 0 changed only what the register said, which is why run 001 still took 4.9 ms
with the change in place, and it would have gone on hiding the real fault - which run 002 then found
as a page fault at an address no register named. The prediction the row made (stage 6 back to about
350 us) is the measurement that refuted it.

It is reverted, not merely re-argued: with the KIQ dequeued in the teardown the register is 0 when
the sample is taken, so upstream's line is correct again. The one place a stale pointer can still be
read is the recovery branch above, and that is where the zero now lives - in the branch that knows
why it is zeroing, rather than in the MQD builder that cannot know.

#### What the host test says about all three

`driver/shim/test/replay_gfx.c` carries two arms, and both are arranged so that the failure
reproduces before the fix:

- `check_rerun()` - teardown, then free and allocate the rings again as the miniport does, then the
  whole bring-up. Before the teardown fix this fails the way unit A did: the MEC resumes at the old
  base plus the old read pointer, the address is in no allocation, and `cp_resume` returns `-62`.
  After it, the second bring-up succeeds in 353 writes against the first run's 354, touches no
  register outside the trace windows the miniport's table is generated from, and the one-write
  difference is `RLC_SPM_MC_CNTL`, which upstream's own `pre_data != data` guard
  (`gfx_v10_0.c:8297`) skips because the field already holds what the first run put there.
- `check_unclean_start()` - the KIQ left active with the MEC halted, which is what 0.6.1's teardown
  leaves behind, then free and allocate again, then bring up. The recovery branch must take exactly
  one stale fetch, inside the ring the last instance left behind, and come back: the bring-up
  succeeds and both the register and the MQD end with a read pointer of 0.

Neither would see anything without the fourth declared model, `backend_add_mec_fetch_state()` in
`driver/shim/test/backend_mem.c`: a replayed register file has no MEC and forgets everything the
moment a register is overwritten. What the model asserts and what was measured for it is written
out in `driver/shim/test/backend_mem.h`, including the one point that is **not** measured - that a
dequeue request serviced by a running MEC clears the copy. That is the assumption both the teardown
and the recovery rest on, and the next hardware run is what tests it.

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

The SDMA side is the same three functions out of `reference/sdma_v5_0.c`, transcribed in
`driver/shim/bc250_sdma.c`: `sdma_v5_0_ring_emit_fence()` at `:1129` (a 32-bit `SDMA_OP_FENCE` with
`MTYPE(3)`, the address, the sequence, and an `SDMA_OP_TRAP` with interrupt context 0 when
`AMDGPU_FENCE_FLAG_INT` is set), `sdma_v5_0_ring_test_ring()` at `:1012` (a `WRITE_LINEAR` of
`0xDEADBEEF` into a scratch dword seeded with `0xCAFEDEAD`, then a poll), and the write pointer.
They close M36's "SDMA: rings programmed and engines released, not tested". The packets are built
out of `third_party/linux-amdgpu/navi10_sdma_pkt_open.h`, imported for this, so no SDMA opcode or
header field is typed here either; `sdma_v5_0.c` includes the same header for the same macros.

Two deviations, both of the kind above:

| Where | What upstream does | Why it cannot work here | What we do instead |
| --- | --- | --- | --- |
| `sdma_v5_0_ring_emit_fence()` (`sdma_v5_0.c:523`) | `BUG_ON()` on a misaligned address, after the header dword is already in the ring | A `BUG()` in a Windows miniport is a bugcheck on a caller's mistake, and a half-written packet is worse than no packet | The same condition, checked before anything is written; `BC250_EINVAL` with the ring untouched |
| `sdma_v5_0_ring_test_ring()` (`sdma_v5_0.c:1012`) | Takes the scratch dword from the device write-back pool (`amdgpu_device_wb_get()`) | There is no write-back pool in this driver: `amdgpu_device.c`'s allocator is Linux's, and ADR 0002 does not import it | `bc250_sdma_fence_page_alloc()` cuts one GTT page into eight-byte slots, exactly as `bc250_gfx.c` already does for the graphics fence. It is deliberately **not** allocated by `bc250_sdma_setup()`: a page the bring-up does not allocate cannot move an address the bring-up programs, so the 354-write comparison cannot be disturbed by a test-only allocation |

One thing deliberately left as it is: `bc250_sdma_ring_test()` fills its slack with single
`SDMA_OP_NOP` dwords rather than the count-carrying burst NOP the engine also accepts. Upstream's
ring test does the same, the packets are identical on the wire, and there is nothing to gain from
differing.

## The compute dispatch (milestone M6)

Nothing from the kernel is imported for it. The kernel has no "run this shader" path that a driver
can call: `amdgpu` dispatches only what user space hands it through a command stream, and the one
shader the kernel owns is in `amdgpu_ucode.c` as firmware. The dispatch therefore comes from
**libdrm's own amdgpu tests**, which do exactly this and nothing else, and which run on gfx10 parts.

Source: `P:\BC-250\ref\libdrm`, tag `libdrm-2.4.114`, commit `b9ca37b31348`.

- The shader binary is imported byte for byte with its MIT notice, never retyped:
  `third_party/libdrm/shader_code_gfx10.h`, hash-verified in both directions
  (`third_party/libdrm/PROVENANCE.md`).
- The PM4 sequence is transcribed into `driver/shim/bc250_dispatch.c` from
  `tests/amdgpu/shader_test_util.c`, with a `file:line` citation at every packet:
  `amdgpu_test_dispatch_memset()` at `:566` and the four functions it calls, `:206-253`,
  `:309-344`, `:403-463`, `:547-565`. Eighteen packets, 72 dwords. What was taken is which registers
  to write, in which order, with which values; every register offset is resolved through
  `gc_10_1_0_offset.h` by `SOC15_REG_OFFSET()`, so no address is typed here, and the host test then
  asserts those resolved offsets against libdrm's literal numbers dword for dword.
- The two magic dwords that cannot be derived from a header are marked as such in the source:
  `COMPUTE_USER_DATA_3 = 0x1104BFAC` (the buffer resource's word 3, `shader_test_util.c:435`) and
  `0x100000` OR-ed into the descriptor's high dword (`STRIDE = 16`, `:433`).

The whole program is measured on this silicon before we emit it: fact M49 ran these 72 dwords and
this shader on unit A under Linux, one workgroup and sixteen, with and without a kernel-prepended
`ACQUIRE_MEM`, and all three filled their buffer
(`evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/dispatch/`). That run went
through an `INDIRECT_BUFFER` with VMID 2 and a user page table, so what remains untested by it, and
ours to be right about, is the two things it could not exercise: submission straight into a
kernel-owned compute ring, and VMID 0 with a GART address.

Three deliberate departures from libdrm, each argued in the header comment of
`driver/shim/include/bc250_dispatch.h`: the packets go straight into the compute ring rather than
into an indirect buffer, both buffers are in GART rather than VRAM, and the cache handling that
`amdgpu_ib_schedule()` would have supplied around an IB is emitted explicitly - an `ACQUIRE_MEM`
copied from `gfx_v10_0_emit_mem_sync()` (`gfx_v10_0.c:9473-9494`) in front, and a
`CS_PARTIAL_FLUSH` before the fence behind.

No register the miniport writes is added by any of this: a dispatch is memory and one doorbell. The
host test asserts that outright - `backend_write_count()` must be 0 across the whole submission - so
`driver/kmd`'s allow-list needs no new entry for M6's dispatch.

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


M349: imported gfx_v10_0_rlc_reset from Linux amdgpu (AMD MIT), preserving both
GRBM_SOFT_RESET.RLC field updates and50us waits. Added an opt-in disabled/busy
RLC plus halted-engine guard before unpublished PSP reload. This is a declared
experimental deviation; ordinary AMD resume does not invoke that callback.

M352: bc250_rlc_reset_readback imports the assertion/read/delay/deassert/read/delay segment of gfx_v10_0_soft_reset (AMD MIT), with the mask restricted to RLC under the existing project halt guard. Gate2 selects this diagnostic variant; ordinary startup and gate1 remain unchanged. Logs are emitted after deassertion. Not a proven reset or upstream ordinary-resume path.


M368 source clarification: bc250_sdma_hw_fini iterates actual SDMA instances.
The v6.18 sdma_v5_0_enable reference computes a bitmask then passes 1 << inst_mask
to gfx_stop; for two instances this selects bit3. The shim preserves its bounded
instance loop, not that shift. No executable change in M368. Ordinary fini is
not the separate stop_queue freeze/UTC_L1-disable reset preparation.


M369: bc250_sdma_quiesce_instance adapts v6.18 sdma_v5_0_stop_queue's register
body; bc250_sdma_unfreeze_instance adapts the unfreeze fragment of restore_queue
(AMD MIT). RLC safe-mode scope/serialization/lifetime are explicit caller duties.
The shim bounds the instance, uses unsigned polling for WDK and BC250_ETIME.
No KMD caller yet; ordinary fini and register replay remain unchanged.


M370: explicit GC10.1 RLC safe-mode scope adapts gfx_v10_0_set/unset_safe_mode
(AMD MIT), requires entry ACK and returns timeout instead of upstream void.
It deliberately requests the scope for enabled RLC independently of cg_flags.
Windows retirement composes this with both SDMA quiescence helpers, then clears
FREEZE while HALT/disabled queues/UTC_L1-off remain verified before paired exit.
This avoids assuming FREEZE persists across PSP reload. Not an upstream reset.


M372: bc250_sdma_soft_reset_instance mechanically extracts Linux v6.18
sdma_v5_0_soft_reset_engine (AMD MIT). Adds fixed-instance bounds, omits DRM debug
printing and substitutes the shim delay; register RMW/readback order is unchanged.
No KMD caller and no hardware reset-completion claim.


M373: Windows stop/reload composes AMD quiescence scope, per-engine reset outside
the scope, then a second full quiescence/unfreeze scope instead of restoring
live queues. All backing retained; post-reset state is reestablished/read back.
Original reset assertion/release reads are logged without additional MMIO.

SDMA virtual IB groundwork: reference/gmc_v10_0.c imported unmodified from
Linux v6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD MIT.
VM flush uses its root-write/invalidate ordering and the imported SDMA register
packet emitters, specialized to GFXHUB/SDMA0 invalidate engine0.

## Startup clock preparation (M429)

PROVENANCE: `reference/cyan_skillfish_ppt.c` and `smu_v11_8_ppsmc.h` are unchanged
AMD MIT sources from Linux v6.18, commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449,
`drivers/gpu/drm/amd/pm/swsmu/{smu11,inc/pmfw_if}`. `cyan_skillfish_clock.inc` is
mechanically extracted by `driver/shim/test/extract_clock.py`: original limits
and complete `cyan_skillfish_od_edit_dpm_table`, including permission notice.
Only the wrapper supplies per-device state and message callbacks; no MMIO access
or hardware ownership is established by this source-only preparation.

- SMU mailbox: AMD MIT `reference/smu_cmn.c`, Linux v6.18 @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`, unchanged. `smu_mailbox.inc` mechanically extracts complete send/response/argument/status bodies; `extract_smu.py --check` verifies them. Port polling uses monotonic time and an iteration bound, with explicit owner and callback-IO errors; debug/no-hardware bypasses are disabled. Generated MP1 byte offsets use regcalc and the existing AMD headers.
