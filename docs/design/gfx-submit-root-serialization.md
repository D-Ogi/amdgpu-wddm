# The root serialization of the gfx submit path, and the VMID pool

Status: option (a) is **implemented in KMD 0.7.214.1 behind `EnableVmidPool` (default on)**. Option (b) is not
implemented. `EnableVmidPool` 0 gives the behaviour of 0.7.213.1.

This document was first written as "F2" against KMD 0.7.196.1. Sections 1 and 3 keep that analysis. Sections 2, 4
and 5 describe what 0.7.214.1 does. Code names refer to the tree of 0.7.214.1.

## 1. What serialized up to 0.7.213.1

`driver/kmd/gfx.c` `SubmitIbLocked` refused a job that did not share the current page-directory root of VMID 1
while another job was still in flight:

```c
if (Gfx->SubmitInFlight != 0 && !GfxFenceArrived(Device, Gfx->SubmitSeq) &&
    (Vmid != 1 || Gfx->SubmitVmid != 1 || Gfx->VmidRoot[Vmid] != RootPhysical)) return STATUS_DEVICE_BUSY;
```

The whole WDDM path submitted at one VMID (`wddm.c` `BC250_WDDM_VMID 1`). The CPU changed the root with
`bc250_gmc_set_vmid_pd` (`driver/shim/bc250_gmc.c`), which writes the per-context page-table-base registers of the
hub and then invalidates that VMID. A root change must not redirect a job that still uses VMID 1, so the driver
waited for idle.

Sessions 313 and 314 measured the cost (Witcher 3 D3D12, register timeline aligned to dxgkrnl ETW to the
microsecond). The game and DWM are different processes with different roots, so every frame alternates two roots
on one VMID. The composition job of DWM runs 0.29 ms (p10-p90 0.26-0.31). The node-0 worker of dxgkrnl offers the
next packet of the game 0.14 ms into it, about 0.13 ms before the completion interrupt of DWM. The refusal costs
4.7 ms per handover at 0.6-1.0 holds a frame: 2.9-4.0 ms of GFX idle a frame.

F1 (KMD 196, the event wake) removed the *wake* latency of that wait, not the wait. The game job still could not
start until the DWM job retired.

Two facts were already true before the pool, and the pool depends on both:

- **Several jobs can be outstanding at once** if they share a root. The fence slot `BC250_SUBMIT_FENCE_SLOT` is a
  monotonic counter read with `bc250_fence_reached`, and the `GfxPending` queue of `wddm.c`
  (`BC250_GFX_PENDING_MAX`, 7 entries) retires them in order.
- **The driver invalidates a VMID by MMIO while a job of that VMID can execute.** The flush is unconditional on
  every submit with a non-zero VMID, and a same-root job is admitted with one in flight. The invalidation picks up a
  leaf-PTE change under an unchanged root.

## 2. Option (a): a VMID per page-table root, recycled on retirement

### The pool

The hardware has 16 VMIDs (`AMDGPU_NUM_VMID`). `gfxhub_v2_0_setup_vmid_config()` inside `bc250_gmc_gart_enable()`
configures the contexts of VMIDs 1..15 once (depth, block size, address range, fault defaults). Only the
page-directory base registers change per submission.

| VMID | Owner | In the pool |
|---|---|---|
| 0 | GART, the system domain | never: `bc250_gmc_set_vmid_pd` refuses it |
| 1 | the single WDDM VMID up to 0.7.213.1 | always, also when the bring-up read finds it programmed |
| 2 | SDMA paging, `BC250_SDMA_PAGING_VMID` | never: `SubmitIbLocked` returns `STATUS_ACCESS_DENIED` |
| 3..15 | none known | yes, unless the bring-up read finds the pair non-zero |

**The bring-up read.** Before the first pool job of a device start, `GfxVmidProbe` reads all 16
`GCVM_CONTEXT*_PAGE_TABLE_BASE_ADDR` pairs through `bc250_gmc_get_vmid_pd` (the offsets come from the hub table of
the shim, not from hand-typed addresses). The pairs of VMIDs 1..15 are in `g_MmioGfxAllow`, the pair of VMID 0 in
the GART table. A VMID in 3..15 that reads non-zero, or that cannot be read, stays out of the pool. The read
happens once per device start and logs this:

```
gfx: VMID bring-up read: non-zero 0x%04lX unread 0x%04lX, pool 0x%04lX (%lu VMIDs), excluded 0x%04lX
gfx: VMID %lu base 0x%llX at bring-up: <GART aperture, reserved | SDMA paging, reserved | VMID 1, kept | excluded from the pool>
```

The second line is written once for each non-zero pair. VMID 1 stays in the pool when it reads non-zero, because
every earlier start of this driver wrote it. A device restart in the same boot finds the VMIDs that the earlier
instance used non-zero and excludes them. The pool then shrinks toward VMID 1 alone, which is the 0.7.213.1
behaviour. The start log shows the pool size, so a trial can see this.

### The table

`BC250_GFX` holds a `BC250_VMID_TABLE` (`driver/kmd/vmid_pool.h`) in place of `VmidRoot[16]`. For each VMID it has
the root, the newest submitted sequence (`LiveSeq`, 0 when retired), the use order, and the tenant: process, first
and last sequence. A ring of 16 `BC250_VMID_TENANT` records keeps the tenancies that ended (section 4).

Locks. Every change happens under `GartLock`, which every submit holds. A change of a root, of a tenant or of the
history also takes `VmidLock`, a spin lock. The fault and timeout reports (DISPATCH_LEVEL) and the `FLUSH_TLB`
builder (which holds `GfxPagingLock`, never `GartLock`) take `VmidLock` to read. The teardown and the power-cycle
reset run with access closed and `GfxPagingLock` exclusive. Both reset the table with `Bc250VmidResetAll`, which
moves every tenancy that ran a job into the history first.

### Choosing a VMID

`SubmitIbLocked` reads the fence slot once, marks every VMID whose `LiveSeq` has arrived as retired
(`Bc250VmidSweep`), then asks `Bc250VmidAdmit`:

1. **The same root is resident.** A pool VMID whose root is `RootPhysical` is used again. A process keeps its VMID
   for as long as it keeps submitting.
2. **A retired VMID.** Otherwise the pool VMID with the oldest use whose last job has retired. Its root is
   programmed with `bc250_gmc_set_vmid_pd`, and the old tenant goes to the history (`Bc250VmidClaim`).
3. **Nothing free.** `STATUS_DEVICE_BUSY`, as before. The caller waits on the retirement event of F1. A retirement
   is what frees a VMID.

A VMID 0 job in flight (the ring test) still holds every other job off, as before. An explicit VMID from the
`IB_AT` escape takes the predicate of 0.7.213.1 unchanged and stays exclusive.

**The rule.** The root of a VMID whose last job has not retired is never rewritten. The fence is global and in
order, so one read of the fence slot answers "has the last job of VMID v retired" for all v. The rule is checked
again after every decision, by `Bc250VmidMayProgram`, before the register write. A decision that breaks it is
refused with `STATUS_DEVICE_BUSY`, counted, and logged once:

```
gfx: VMID %lu root 0x%llX -> 0x%llX REFUSED: its job %lu has not retired (fence %lu)
```

The chooser cannot produce such a decision (section 5 has the model check). The check is there for the next
change to this code.

**Deviation from the F2 text: the order is the order of use, not of claim.** The F2 text asked for "the least
recently claimed" VMID. The table stamps the order on every claim and on every submit. With claim order, a VMID
that DWM claimed early and uses every frame would be the oldest claim and the first to recycle as soon as it
retires between two frames. With use order, the VMID that has not submitted for the longest time goes first, and
DWM keeps its VMID.

### The per-submit invalidation stays

`bc250_gmc_set_vmid_pd` is still called on every submit, for the chosen VMID, with the root unchanged or not. The
invalidation is what picks up a leaf-PTE change of the paging path under the same root. The pool never changes the
root of a live VMID, so it needs no pipeline sync before a root change.

### The VMID end to end

- `wddm.c` `WddmSubmitHardware` asks for `BC250_VMID_AUTO`. `GfxSubmitIb` returns the VMID it chose, and the
  queue entry (`BC250_GFX_COMPLETION.Vmid`), the `wddm: fence %u on the gfx ring` line and the timeout report
  keep it.
- `bc250_gfx_submit_job` puts the VMID into the control word of the IB packet
  (`PACKET3_INDIRECT_BUFFER__VMID`, `bc250_gfx_emit_ib`). The CP fetches the IB, and the job makes every access,
  through that VMID.
- The `BC250_PJ_GFX_SUBMIT` journal record carries the VMID in `Valid` (0 in the records of earlier drivers).
  `bc250kmd_cli journal` prints it. The hot line `gfx: job seq %lu vmid %lu fence ...` carries it too; the node
  left that line to keep it within 159 characters, and the journal record keeps the node.
- `ProgressSiteVmFlush` records the VMID as its input, so a dump of a hang inside the flush names it.

### FLUSH_TLB

`DXGK_OPERATION_FLUSH_TLB` names a root. `wddm.c` resolves it with `VidMmRootPhysical` (0 when it does not
resolve), and `GfxPagingBuildFlush` builds the invalidations. With `EnableVmidPool` 0 it invalidates VMID 1, as
before. With the pool it invalidates `Bc250VmidFlushMask`: every VMID that holds the root, every pool member and
every other VMID that holds a root at all.

**Deviation from the F2 text: a superset, not the holders of the root.** The VMIDs that hold the root when the
paging buffer is built are not always the VMIDs that hold it when SDMA executes it. In between, the VMID of the
root can retire and recycle, and the root can be claimed again on another VMID, whose TLB can then cache a
translation that the page-table writes of the same buffer change. The superset does not depend on that timing.
The cost is at most 14 invalidations of 15 dwords each for one `FLUSH_TLB`. The one logged rate on file is 47863
`FLUSH_TLB` in 3909 s, about 12 a second (`evidence/linux/2026-09-24-E29-sdma-reset/windows-before.log`). The
build is all or nothing: if the whole set does not fit, the answer is `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER`
with nothing published, and the next buffer starts the whole set again.

### Every use of the old fixed VMID

| Use up to 0.7.213.1 | 0.7.214.1 |
|---|---|
| `wddm.c` `#define BC250_WDDM_VMID 1u` | removed |
| `wddm.c` `WddmSubmitHardware`: `GfxSubmitIb(Device, BC250_WDDM_VMID, ...)` | `BC250_VMID_AUTO`, the chosen VMID returned |
| `wddm.c` `WddmSubmitHardware`: the `vmid %u` of `wddm: fence %u on the gfx ring` | the chosen VMID |
| `wddm.c` FLUSH_TLB: `GfxPagingBuildFlush(..., BC250_WDDM_VMID, ...)` | the resolved root; the fan-out above |
| `gfx.c` `BC250_GFX.VmidRoot[16]` | `BC250_GFX.Vmid.Root[16]` with the rest of the table |
| `gfx.c` `SubmitIbLocked`: the refusal predicate on `VmidRoot` | the same predicate with the gate at 0; `Bc250VmidAdmit` with the pool |
| `gfx.c` `SubmitIbLocked`: `VmidRoot[Vmid] = RootPhysical` | `Bc250VmidClaim` under `VmidLock` |
| `gfx.c` `TearDown` and `GfxPowerRetainedLocked`: zero `VmidRoot` | `Bc250VmidResetAll` |
| `driver/kmd/test/gfx_pipeline_test.c`, `gfx_retained_test.c` | follow the new types; the pipeline test checks the VMID in the queue and in the timeout report |
| `experiments/E27-m9-inference/paging-route-test-suffix.c` (`shim-paging`) | the real `GfxPagingBuildFlush`, gate 0 and the fan-out |

## 3. Option (b): the flush on the ring (not implemented)

Upstream does the root change and the invalidation in the command stream: `gmc_v10_0_emit_flush_gpu_tlb`
(`driver/amdgpu-import/reference/gmc_v10_0.c`) emits two `WREG` packets for the page-table-base pair of the context
and one `reg_write_reg_wait` for the invalidate request and acknowledge, and `gfx_v10_0_ring_emit_vm_flush`
follows it with `PACKET3_PFP_SYNC_ME`. The packets execute in ring order, so the CP serializes the root change
against the previous job, and one VMID is enough.

Against it, now:

- It changes the frame of `bc250_gfx_submit_job`, a fixed, logged, known-good packet sequence that a dump of a
  0x116 is read against.
- It needs `vm_inv_eng`, `eng_distance`, the hub register bases and the acknowledge semantics to be correct in
  emitted packets, not in MMIO writes that can be read back. A wrong acknowledge mask is a CP that waits forever:
  a TDR, and no engine reset on this part (facts M53).
- The invalidation engine is shared with the CPU path (`bc250_gmc_flush_gpu_tlb`), and the shim has no equivalent
  of the upstream `adev->gmc.invalidate_lock`.

(b) remains the way to one VMID and no CPU poll on the submit path, after the ownership of the invalidation
engine is settled.

## 4. VM fault attribution

Up to 0.7.213.1 every application job ran at VMID 1, so the VMID of `GCVM_L2_PROTECTION_FAULT_STATUS` and of the
UTCL2 vector named no process. With the pool, a VMID maps to a root while it is not recycled, and the root maps to
a process. A fault latched on a VMID that was recycled before the latch was read needs the history.

0.7.214.1 keeps a ring of 16 ended tenancies: VMID, root, process, first and last sequence. `GfxVmidReport` prints
the current tenant and the newest ended tenant of a VMID:

```
ih: GPU FAULT vector vmid %lu now: root 0x%llX pid %lu seq %lu-%lu
ih: GPU FAULT vector vmid %lu before: root 0x%llX pid %lu seq %lu-%lu
ih: GPU FAULT latch vmid %lu now: ...                   (only when the latch names another VMID)
wddm: timeout job vmid %lu now: ...
wddm: timeout latch vmid %lu now: ...
```

`IhFaultReport` calls it for the VMID of the vector and, when it differs, for the VMID of the latch. The watchdog
calls it for the VMID of the timed-out job and, in the register snapshot, for the VMID of the latch. The line
`wddm: timeout seq %lu fence %u node %u vmid %lu ctx ...` names the VMID of the job.

## 5. Risks, and what tests them

| Risk | Consequence | Mitigation | Test |
|---|---|---|---|
| A VMID recycled while a job of its old root still runs | wrong translations, VM fault, 0x116 | the retirement test of the chooser; the rule check before every root write, logged once | `vmid-pool`: an exhaustive model check of every interleaving of submit, retire and root choice for pools of 1, 2, 3 and 2 far-apart VMIDs (2.2 million interleavings), and 1.28 million seeded random steps across the sequence wrap; the safety property is "every job in flight still finds its root at its VMID" |
| The chooser is changed to ignore retirement | the same | the negative control | `vmid-pool-ignore-retirement` must fail |
| A pool VMID used by firmware (PSP, RLC) | corruption outside our contexts | the bring-up read; a non-zero VMID in 3..15 stays out | `vmid-pool`: the probe cases |
| `FLUSH_TLB` misses a VMID that holds the root | stale translations after a paging unmap | the superset of section 2 | `shim-paging`: the real `GfxPagingBuildFlush` with gate 0 (VMID 1 alone, byte for byte) and with the pool (the fan-out, the recycle race, all or nothing) |
| A fault latched on a recycled VMID | a fault nobody can attribute | the history of section 4 | `vmid-pool`: the history cases |
| `EnableVmidPool` 0 is not 0.7.213.1 | a bisect switch that lies | the gate-0 path keeps the old predicate at the old point | `vmid-pool`: gate-0 identity against the old predicate (120 cases); `gfx-pipeline`, `gfx-retained` |
| Fourteen VMIDs used up by many processes | `STATUS_DEVICE_BUSY`, the behaviour of 0.7.213.1 | nothing; F1 makes the wait cheap | the summary counts `busy` |

## 6. Gate, counters, rollback

`EnableVmidPool` is a REG_DWORD under `Services\bc250kmd\Parameters`, read once in `GfxStart`. Absent or any value
other than 0 opens the pool. The INF and the release installer write 1. 0 gives 0.7.213.1: every job at VMID 1,
the old refusal at the old point, no bring-up read, and `FLUSH_TLB` on VMID 1 alone.

The start log says `gfx: VMID pool on (EnableVmidPool)` or `gfx: VMID pool off (EnableVmidPool 0): every job at
VMID 1`. The summary has three lines:

```
wddm summary: VMID pool on, members 0x%04lX, excluded 0x%04lX
wddm summary: VMID pool: %lu claims, %lu reuses, %lu busy, %lu rule refusals
wddm summary: VMID pool FLUSH_TLB: %lu built, %lu VMID invalidations
```

`rule refusals` must be 0. Any other value is a defect. To roll back, set `EnableVmidPool` to 0 and restart.
