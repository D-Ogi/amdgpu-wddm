# F2: removing the root serialization of the gfx submit path

Status: design only. **Not implemented in KMD 0.7.196.1**, and the registry value named below does not exist
yet. The reasons are in "Why not in 196" at the end. KMD 196 implements F1 (the event wake) alone, so that the
lab session that prices F1 measures one change.

Written against `kmd196-submit-wake` at the tree of KMD 0.7.196.1. Line references are that tree.

## 1. What serializes today

`driver/kmd/gfx.c` `SubmitIbLocked` refuses a job that does not share VMID 1's current page-directory root
while another job is still in flight:

```c
if (Gfx->SubmitInFlight != 0 && !GfxFenceArrived(Device, Gfx->SubmitSeq) &&
    (Vmid != 1 || Gfx->SubmitVmid != 1 || Gfx->VmidRoot[Vmid] != RootPhysical)) return STATUS_DEVICE_BUSY;
```

The whole WDDM path submits at one VMID (`wddm.c` `BC250_WDDM_VMID 1`), and the root is changed from the CPU by
`bc250_gmc_set_vmid_pd` (`driver/shim/bc250_gmc.c:251`), which writes the hub's per-context page-table-base
registers and then invalidates that VMID. A root change must not redirect a job that is still using VMID 1, so
the driver waits for idle instead.

Consequence measured in session 313 (Witcher 3 D3D12, register timeline aligned to dxgkrnl ETW): the game and
DWM are different processes with different roots, so every frame alternates two roots on one VMID, and the GFX
pipe is idle 3.1 ms per frame with the game's packet accepted by dxgkrnl but not yet on the ring. F1 removes the
*wake* latency of that wait (sleep on a clock tick -> wake on the fence). It does not remove the wait itself:
the game job still cannot start until the DWM job retires.

Two facts that matter to everything below, both already true in the deployed driver:

- **Several jobs may already be outstanding at once**, as long as they share a root. `Gfx->SubmitSeq` holds the
  newest sequence, the fence slot `BC250_SUBMIT_FENCE_SLOT` is a monotonic counter read with
  `bc250_fence_reached`, and `wddm.c`'s `GfxPending` queue (`BC250_GFX_PENDING_MAX` entries) retires them in
  order. The `gfx: pipeline queued seq%lu prior%lu ... overlap%u` line exists to witness exactly that.
- **The driver already invalidates a VMID by MMIO while a job of that VMID may be executing.** The flush at
  `gfx.c` ~1080 is unconditional on every non-zero-VMID submit, and the refusal above admits a same-root job
  with one in flight. So "CPU invalidate concurrent with CP execution" is not a hazard F2 introduces; it is the
  hazard the current driver already takes, on purpose, because the invalidation is what picks up a leaf-PTE
  change made under an unchanged root (the comment at `gfx.c` ~1075).

## 2. Option (a): a VMID per page-table root, recycled on retirement

The hardware has 16 VMIDs (`AMDGPU_NUM_VMID`, `driver/shim/include/amdgpu.h:207`). Their contexts - depth,
block size, address range, fault defaults - are already configured for VMIDs 1..15 by
`gfxhub_v2_0_setup_vmid_config()` inside `bc250_gmc_gart_enable()`, and none of that changes per submission
(the comment in `bc250_gmc_set_vmid_pd`). Only the page-directory base registers are per-submission state.

Reserved and therefore outside the pool:

| VMID | Owner | Why it is reserved |
|---|---|---|
| 0 | GART / system domain | depth 0, the flat aperture every driver-owned buffer is addressed through; `bc250_gmc_set_vmid_pd` refuses it outright |
| 2 | SDMA paging | `BC250_SDMA_PAGING_VMID` (`shim/include/bc250_sdma.h:154`); node 1's channel programs its root per paging buffer, `SubmitIbLocked` returns `STATUS_ACCESS_DENIED` for it |

That leaves VMIDs 1 and 3..15 - fourteen - for application roots. No firmware use of any other VMID was found
in the imported sources; before implementing, confirm it by reading all 16 `GCVM_CONTEXT*_PAGE_TABLE_BASE_ADDR`
pairs once at the end of bring-up and logging the non-zero ones. If PSP or RLC holds one, it will show there.

### State

In `BC250_GFX`, beside the existing `ULONGLONG VmidRoot[16]`:

```c
ULONG VmidLastSeq[16];      // the newest sequence submitted on this VMID, 0 = never used
ULONG VmidRecycleOrder[16]; // claim order, for the LRU choice among free VMIDs
```

`VmidRoot`/`VmidLastSeq` are written only under `GartLock`, which every submit already holds, so no new lock.
`GfxTearDown` already zeroes `VmidRoot` (`gfx.c` ~3453 "force VMID reprogramming on the next job"); it must zero
the two new arrays in the same place and for the same reason.

### Choosing a VMID

Under `GartLock`, given `RootPhysical`:

1. **Same root already resident.** A pool VMID with `VmidRoot[v] == RootPhysical` -> use `v`. This is today's
   fast path and keeps a process on one VMID for as long as it keeps submitting, which is what makes the fault
   attribution in section 4 useful.
2. **A retired VMID.** Otherwise the least recently claimed pool VMID whose last job has retired -
   `VmidLastSeq[v] == 0 || GfxFenceArrived(Device, VmidLastSeq[v])`. Program its root
   (`bc250_gmc_set_vmid_pd`), set `VmidRoot[v]`, bump its recycle order.
3. **Nothing free.** `STATUS_DEVICE_BUSY`, exactly as today. The caller then waits on F1's retirement event,
   which is correct unchanged: a retirement is precisely what frees a VMID.

The recycle rule is the correctness core: **a VMID's root may be rewritten only after that VMID's last
submitted job has retired.** `VmidLastSeq[v]` plus the monotonic fence answers that with one memory read and no
new hardware state. Because the fence is global and in-order, `GfxFenceArrived(Device, VmidLastSeq[v])` is true
for every job that was submitted before the newest retired one, so step 2 never recycles a live VMID.

### The per-submit invalidation stays

`bc250_gmc_set_vmid_pd` is still called on every submit, for the chosen VMID, root unchanged or not. Dropping
it when the root matches would lose the leaf-PTE change that the paging path may have made under that same root
- the exact thing the comment at `gfx.c` ~1075 says the flush is for. Cost: one 32-bit pair of register writes
and one request/ack poll per submit, which is what the path pays today.

This is also why option (a) does not need a pipeline sync before the root change: it never changes a live
VMID's root, so there is nothing to sync against. The `PACKET3_PFP_SYNC_ME` that `bc250_gfx_submit_job` already
emits (`shim/bc250_gfx.c:1813`, the PFP half of `gfx_v10_0_ring_emit_vm_flush`) stays as it is.

### What else moves

- `Gfx->SubmitVmid` becomes per-job information only (the journal and the timeout snapshot), not a gate.
- `wddm.c` `WddmSubmitHardware` passes `BC250_WDDM_VMID` today; it would pass "pool" and let gfx.c choose. The
  `BC250_PJ_GFX_SUBMIT` journal record and the `gfx: job seq ...` line must gain the chosen VMID, or a dump can
  no longer tell which context a sequence ran on.
- `GfxPagingBuildFlush` is called with `BC250_WDDM_VMID` from `wddm.c` ~4488 for `DXGK_OPERATION_FLUSH_TLB`.
  With a pool, a TLB flush operation has to flush every pool VMID that currently holds the requesting root, not
  a fixed 1. Getting this wrong is a stale-translation bug, not a performance regression, so it is the first
  thing a reviewer should check in the implementation.
- DPM: `DpmBusyBegin/End` already pair against "the newest outstanding sequence retired" and need no change.

### Expected effect

The game and DWM jobs land on the ring back to back; the 3.1 ms class of session 313 goes to the ring's own
0.63 ms Start-to-ring, and the remaining idle is the 0.94 ms completion-report class plus whatever the CP needs
between frames. This is a prediction, not a measurement.

## 3. Option (b): the flush on the ring

Upstream does the root change and the invalidation in the command stream:
`gmc_v10_0_emit_flush_gpu_tlb` (`driver/amdgpu-import/reference/gmc_v10_0.c:374`) emits two `WREG` packets for
the context's page-table-base pair and one `reg_write_reg_wait` for the invalidate request/ack, and
`gfx_v10_0_ring_emit_vm_flush` (`reference/gfx_v10_0.c:8767`) follows it with `PACKET3_PFP_SYNC_ME`.
`emit_pasid_mapping` and `PACKET3_INVALIDATE_TLBS` are the PASID-keyed variants, used for KFD, not needed here.

Because the packets execute in ring order, the CP itself serializes the root change against the previous job: no
CPU wait, no idle requirement, and one VMID is enough. That is strictly better than (a) in throughput terms and
is the upstream-shaped answer.

Against it, here, now:

- It changes `bc250_gfx_submit_job`'s frame, which is a fixed, logged, known-good packet sequence. The frame is
  what a dump of a 0x116 is read against (the `gfx: job frame C0004200 ...` line exists for that).
- It needs `vm_inv_eng`, `eng_distance`, the hub register bases and the per-engine ack semantics to be correct
  in emitted packets rather than in MMIO writes that can be read back. A wrong ack mask is a CP that waits
  forever, which on this part means a TDR and no engine reset (facts M53).
- The invalidation engine registers are shared with the CPU path (`bc250_gmc_flush_gpu_tlb`), and the shim has
  no equivalent of upstream's `adev->gmc.invalidate_lock` (`bc250_gmc.c:149`). On-ring and MMIO invalidation
  would then be two unsynchronized users of one engine. Node 1's SDMA path already emits its own
  `bc250_sdma_emit_vm_flush`, so this question has to be answered for (b) whether or not node 0 moves.

Recommendation: (a) first, because it is additive and every step of it can be read back from a register; (b)
afterwards, as the way to get down to one VMID and no CPU poll on the submit path, with the invalidation-engine
ownership settled first.

## 4. VM fault attribution

Today every application job runs at VMID 1, so the `VMID` field of `GCVM_L2_PROTECTION_FAULT_STATUS`
(`ih_fault.h:50`) and of the UTCL2 vector says nothing about which process faulted; the journal's
`BC250_PJ_GFX_SUBMIT` record is the only link, and it is a link to a sequence, not to the VMID in the latch.

Under (a) this improves: a VMID maps to a root for as long as it is not recycled, and the root maps to a
process (`Context->CreatorProcessId` is already in the submit identity). The implementation therefore has to
keep a small retired-VMID history - root, process, the sequence range it covered - so that a fault latched on a
VMID that has since been recycled is still attributable. Without that history, (a) makes attribution *worse*
than today in exactly the case that matters, the one where a fault stopped the engine and the next submit
recycled the VMID before anybody read the latch.

Under (b) with one VMID, attribution stays where it is: the journal record.

## 5. Risks

| Risk | Consequence | Mitigation |
|---|---|---|
| A VMID recycled while a job of its old root is still fetching | wrong translations, VM fault, 0x116 | the retirement test of section 2 step 2; a `C_ASSERT`-grade invariant check in the claim path, logged once |
| A pool VMID used by firmware (PSP, RLC) | corruption outside our contexts | read all 16 page-table-base pairs at the end of bring-up, log the non-zero ones, start the pool from what that shows |
| `FLUSH_TLB` flushing one VMID when several hold the root | stale translations after a paging unmap | flush every pool VMID holding that root; this is the item to review first |
| Fault latched on a VMID that was recycled before the latch was read | a fault nobody can attribute | the retired-VMID history of section 4, in the same change, not later |
| Fourteen VMIDs exhausted by many processes | back to `STATUS_DEVICE_BUSY`, i.e. today's behaviour | nothing needed; F1's event wake makes that wait cheap |

## 6. Why not in 196

- F1 is measurable on its own and the lead needs one lab session to price it. A VMID pool in the same package
  would change the same counters for a second reason.
- (a) is additive but not small: a chooser, two new arrays, the `FLUSH_TLB` fan-out, the journal/VMID plumbing
  and the retired-VMID history for fault attribution. Each of those is a correctness surface whose failure mode
  is a 0x116 on the lab, not a slow frame.
- The one unknown that must be settled before any of it is which VMIDs the firmware uses. That is a read-only
  bring-up measurement and it does not exist yet.

When it is implemented, it goes behind `EnableVmidPool` (DWORD, service parameters, **default 0**), read once at
start like every other gate in this driver, with the chosen VMID in the journal record and in the
`gfx: job seq ...` line so that a run with the gate open can be told from one without it in a log alone.
