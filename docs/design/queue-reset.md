# Stage 2 of the hang recovery: KIQ queue resets for node 2 and node 0

Status: IMPLEMENTED. Node 2 (compute) runs it in both flavours. Lab trial Q2 measured it (M959, section 11).
Node 0 (GFX) runs it only with the lab selector, and no trial has measured that half yet. Stage S2 of
[the TDR recovery plan](tdr-recovery-plan.md#52-stages). The section "Stage 2" of [hang-recovery.md](hang-recovery.md)
points here.

## 1. Problem

Stage 1 (`GfxSoftRecoverRing`, gfx.c) writes the broadcast wave kill `SQ_CMD` for 10 ms. When the ring does not
retire, `ResetEngine` fails, dxgkrnl promotes the timeout to an adapter reset, and S1 refuses it because the
witnesses read a busy engine. The result is 0x116 with argument 3 `0xC0000001`. Two tester.36 candidate dumps of
2026-11-02 have this shape:

| Dump | Node | VMID | Witnesses after the kills | What stage 1 met |
|---|---|---|---|---|
| A7 call 10, `boot-20261102T230534Z` | 2 (compute) | 15 | `GRBM_STATUS 0xA0003028`, `CP_STAT 0`, `CP_CPC_STATUS 0xA0000001` | 0 waves before the kill. The MEC is busy with no wave: a CP-side wait. |
| 3DMark SystemInfo scan, `boot-20261102T232052Z` | 0 (GFX) | 13 | `GRBM_STATUS 0xA0003028`, `CP_STAT 0x90018E00`, `CP_CPC_STATUS 0` | 64 waves before the kill. 10.09 s before the timeout, a no-retry GPU fault of client CPG (cid 6), write, VA `0x1001C8000`, VMID 13. |

`SQ_CMD` ends waves. It does not end a CP packet that waits, and it does not end a CP that is stalled on a
no-retry fault. Both need the queue taken off the hardware queue descriptor (HQD) and set up again. amdgpu does
that with the per-queue resets of `gfx_v10_0.c`, and M944 measured both on unit A under Linux.

## 2. Sources

`ref/linux-src` at `7d0a66e4b` (the commit that `driver/amdgpu-import/reference` also records).

| Step | amdgpu | Our existing code |
|---|---|---|
| Compute queue reset | `gfx_v10_0_reset_kcq`, `gfx_v10_0.c:9578-9646` | none |
| GFX queue reset | `gfx_v10_0_reset_kgq`, `gfx_v10_0.c:9511-9576` | none |
| `UNMAP_QUEUES` packet | `gfx10_kiq_unmap_queues`, `gfx_v10_0.c:3766` | `bc250_kiq_unmap_queues` (RESET_QUEUES only), `bc250_gfx.c:1444` |
| `MAP_QUEUES` packet | `gfx10_kiq_map_queues`, `gfx_v10_0.c:3726` | `bc250_kiq_map_queues`, `bc250_gfx.c:1401` |
| Queue set-up again, compute | `gfx_v10_0_kcq_init_queue(ring, true)`, `gfx_v10_0.c:7169` | `bc250_kcq_init_queue` (cold arm), `bc250_gfx.c:1279` |
| Queue set-up again, GFX | `gfx_v10_0_kgq_init_queue(ring, true)`, `gfx_v10_0.c:6844` | `bc250_kgq_init_queue` (cold arm), `bc250_gfx.c:1360` |
| Register write and register wait on the KIQ | `gfx_v10_0_ring_emit_wreg`, `gfx_v10_0_ring_emit_reg_wait:9026`, `gfx_v10_0_wait_reg_mem:4011` | the emit helpers of `bc250_gfx.c:1828-1880` |
| Ring test | `gfx_v10_0_ring_test_ring`, `gfx_v10_0.c:4033` | `bc250_gfx_ring_test`, `bc250_gfx.c:1468` |
| Begin and end of a reset | `amdgpu_ring_reset_helper_begin/_end`, `amdgpu_ring.c:794-825` | the stage-1 commit in wddm.c (gate, fence, VMID) |
| Per-queue reset allowed on gfx10 | `gfx_v10_0.c:4955-4961` | n/a |
| Ring positions reset | `gfx_v10_0_kcq_init_queue` restore arm | `GfxResetRetainedRing`, gfx.c |

Register names and addresses, from `python tools/regcalc/regcalc.py lookup` at `9b9cbe11`:

| Name | BAR5 offset | Use |
|---|---|---|
| `mmCP_VMID_RESET` | `0x0C2CC` | GFX queue reset request (KIQ write), then wait for 0 |
| `mmCP_HQD_ACTIVE` | `0x0C82C` | compute HQD state, bit 0, under the queue select |
| `mmCP_HQD_DEQUEUE_REQUEST` | `0x0C884` | fallback only, see 5.3 |
| `mmCP_HQD_PQ_RPTR` | `0x0C84C` | witness, under the queue select |
| `mmCP_HQD_PQ_WPTR_LO` | `0x0C8FC` | witness, under the queue select |
| `mmCP_GFX_HQD_ACTIVE` | `0x0C380` | GFX HQD state, witness |
| `mmCP_GFX_HQD_RPTR` | `0x0C3A0` | witness |
| `mmGRBM_GFX_CNTL` | `0x08088` | the queue select (`nv_grbm_select`) |
| `mmRLC_SAFE_MODE` | `0x3B014` | safe mode around the HQD reads |
| `mmGRBM_STATUS` | `0x08010` | witness |
| `mmCP_STAT` | `0x08680` | witness, GFX CP |
| `mmCP_CPC_STATUS` | `0x08210` | witness, MEC |
| `mmGCVM_L2_PROTECTION_FAULT_STATUS` | `0x0A130` | witness of the fault class |
| `mmSQ_CMD` | `0x08DEC` | stage 1, unchanged |

## 3. Why the facts against stage 2 no longer hold

hang-recovery.md gives three facts against a queue reset. None of them describes the step of this design:

- M44: a second bring-up in one device session fails at the KIQ. This design does not bring the KIQ up again. It uses the
  live KIQ to unmap and map one client queue, which every device session already does at bring-up and at `hw_fini`.
- M53 and M55: amdgpu's full GPU recovery and a second init hang unit A. This design does neither.
- M944 (2026-11-01) measured on unit A: `gfx_v10_0_reset_kgq` ends a timed-out GFX job in 0.28 ms and
  `gfx_v10_0_reset_kcq` ends a compute spin in 0.18 ms, with no full recovery, and later GFX, compute and SDMA
  work passes.

What M944 does not cover: a CP-side wait on the MEC with no wave (A7), and a CP stalled on a no-retry fault (3DMark).
The lab trials of section 8 measure those two.

## 4. Where it runs

- In `ResetEngine` (wddm.c), when the stage-1 kill loop returns `NOT_DRAINED` and `HangRecoveryMode` is not 0:
  for node 2 and node 0 in both flavours, when the device has that queue reset (`Bc250QueueResetWanted`,
  section 12). Until tester.38 node 0 needed the lab selector `LabQueueReset` 1. Since 2026-11-08,
  BD-180 point 3 closes in tester.38, so `BC250_QRESET_GFX_DEFAULT` is 1. If the lab trial Q3 refutes the
  node-0 reset, that one constant brings the lab-only rule back.
  dxgkrnl calls `ResetEngine` at PASSIVE_LEVEL in first-level synchronization, so no other DDI of the adapter runs.
- In `ResetFromTimeout` (since tester.38, section 13), in `AdapterRecoveryMode` 2, after the S1 wave kills and
  before the S1 witnesses: the queue reset of each ring that still has an unretired sequence (nodes 0 and 2 through
  the KIQ, node 1 through SDMA0). S1 then reads its witnesses and keeps its rule: success only when they prove idle.
- The selector of node 0 is `LabQueueReset` (REG_DWORD under Parameters, read at DriverEntry, lab flavour only,
  the pattern of `LabScaledPlane`), not a value 2 of `HangRecoveryMode`. A new allowed value of a catalog setting
  changes the catalog identity, and a KMD of another catalog fails to begin over the installed package. Node 2
  needs no new setting: it follows `HangRecoveryMode`, which the catalog already has. `HangRecoveryMode` 0 turns
  stage 2 off for both nodes.
- Node 1 (paging, SDMA) has no reset in `ResetEngine`: dxgkrnl follows an engine reset that affects a paging packet
  with an adapter-wide reset (tdr-changes-in-windows-8.md, step 9), so `ResetEngine` refuses node 1 and
  `ResetFromTimeout` resets SDMA0 (section 13).

## 5. Order of operations

All steps run under `GartLock`, with the GFX sequence installed as `adev->backend`, as `GfxSoftRecoverRing`
does. The submission gate of the node is already closed by stage 1, and it stays closed until the commit in step 9.
Every wait has a bound. The first failure ends the attempt (section 6).

### 5.1 Compute queue (node 2), after `gfx_v10_0_reset_kcq`

1. Witnesses before: `GRBM_STATUS`, `CP_CPC_STATUS`, and, under `nv_grbm_select(me, pipe, queue)`,
   `CP_HQD_ACTIVE`, `CP_HQD_PQ_RPTR`, `CP_HQD_PQ_WPTR_LO`. One log line.
2. KIQ: `UNMAP_QUEUES` with `RESET_QUEUES` for the ring (`bc250_kiq_unmap_queues`, 6 dwords), commit,
   `bc250_gfx_ring_test(kiq)`. A KIQ that does not answer ends the attempt.
3. `RLC_SAFE_MODE` enter, select the queue, poll `CP_HQD_ACTIVE` bit 0 until it is 0, bound `usec_timeout`
   (100 ms, amdgpu's `AMDGPU_MAX_USEC_TIMEOUT`), select 0, safe mode exit. Select and safe mode are restored on
   every exit of this step.
4. Clean queue state: the restore arm of `kcq_init_queue`. The ring has no backup MQD, so the MQD is built again
   from the retained ring descriptor (`bc250_kcq_init_queue`), after `GfxResetRetainedRing` sets `wptr` 0 and
   clears the ring, and the write-back `wptr` slot is set to 0. The cold arm reads `CP_HQD_PQ_RPTR` into the MQD:
   after step 3 the HQD is inactive, and the new helper writes 0 into the MQD read pointer instead of the read
   value, as the restore arm of amdgpu has a clean backup.
5. KIQ: `MAP_QUEUES` for the ring (7 dwords), commit, `bc250_gfx_ring_test(kiq)`.
6. Proof on the queue itself: `bc250_gfx_ring_test(compute ring)`: a fresh fence through the new queue.

### 5.2 GFX queue (node 0), after `gfx_v10_0_reset_kgq`

1. Witnesses before: `GRBM_STATUS`, `CP_STAT`, `CP_GFX_HQD_ACTIVE`, `CP_GFX_HQD_RPTR`,
   `GCVM_L2_PROTECTION_FAULT_STATUS`. One log line.
2. KIQ, one submission of 5 + 7 + 7 dwords:
   - write `CP_VMID_RESET` = `RESET_REQUEST(1 << vmid)` with `PIPE0_QUEUES(1 << queue)` (or `PIPE1_QUEUES`
     for pipe 1).
   - `WAIT_REG_MEM` on the MQD dword `cp_gfx_hqd_active`, memory space, equal, reference 0, mask 1, poll 0x20.
   - register wait `CP_VMID_RESET` == 0, mask `0xFFFFFFFF`.
   Commit, `bc250_gfx_ring_test(kiq)`. The ring test of the KIQ is the proof that the KIQ passed both waits.
3. Clean queue state: the reset arm of `kgq_init_queue`: the doorbell range again when this is ring 0
   (`bc250_cp_gfx_set_doorbell`), the MQD built again from the retained descriptor, `wptr` 0, ring cleared.
4. KIQ: `MAP_QUEUES` (engine select 4), commit, `bc250_gfx_ring_test(kiq)`.
5. Proof on the queue itself: `bc250_gfx_ring_test(gfx ring)`.

### 5.3 Fallback (in the code since lab Q2)

When the poll of step 3 of 5.1 times out, the code reads the HQD witness again and runs the direct handshake of
kfd's `kgd_hqd_destroy` (`amdgpu_amdkfd_gfx_v10.c:606-619`): `CP_HQD_DEQUEUE_REQUEST` = 1 (DRAIN_PIPE), poll
`CP_HQD_ACTIVE` for the same bound, then `CP_HQD_DEQUEUE_REQUEST` = 0. The KMD already runs that handshake for the
KIQ (`bc250_kiq_dequeue`). Still under the queue select and in RLC safe mode. When the HQD goes inactive, the reset
goes on with step 4. When it stays active, step 3 fails as before. amdgpu's queue reset does not have this step.
Lab Q2 needs it: after the KIQ unmap the HQD stayed active for the whole bound, and after this write it went
inactive in 3 to 5 us (M959).

### 5.4 After the queue reset (both nodes)

7. Wave proof for the hung VMID: one pre-commit wave capture (`GfxCaptureWaves`, which works again after
   `381a5867`). It must report no wave of the hung VMID. A failed capture is a failed proof.
8. VMID: the hung VMID's tenancy goes to the history and the VMID is reprogrammed on its next job, with a TLB
   invalidation of that VMID, as the retained resume does with `Bc250VmidResetAll` for all of them.
9. The stage-1 commit under the WDDM lock: the node fence advances to the last submitted sequence of the ring, the
   aborted fence id is the hung sequence, the gate opens. Packets queued behind the hung one on the same ring are
   lost with the cleared ring. They are reported as aborted, as amdgpu reports all fences of the guilty context.
   (amdgpu re-emits the packets of other contexts. dxgkrnl's engine reset resubmits what it still has, so the KMD
   does not copy ring contents.)

## 6. What proves "stopped"

`ResetEngine` returns success, and S1 accepts the adapter path, only when all of these hold:

- the HQD of the queue is inactive: compute by a read of `CP_HQD_ACTIVE` == 0 (step 5.1.3). GFX by the KIQ having
  passed the wait on `cp_gfx_hqd_active` and on `CP_VMID_RESET` == 0 (the KIQ ring test of 5.2.2).
- the queue fetches again from a clean ring: the ring test of the queue retires a fresh fence (5.1.6, 5.2.5).
- no wave of the hung VMID is on the hardware (5.4.7).
- for S1 only: its existing witnesses after the reset (`GRBM_STATUS`, `CP_STAT`, `CP_CPC_STATUS`, ring pointers,
  SDMA) read idle, with the one change that a ring whose queue reset passed counts by its new positions.

Together these say that the queue cannot fetch the old packets and that no shader of that VMID runs, so no
engine of the GFX block accesses the VMID's memory. That is the statement that S1 needs before dxgkrnl frees it.

## 7. Failure handling

- Any failed step: log the step, the bound and the register values. Do not run any later step. Return the stage-1
  failure, so the path is today's (0x116). No second attempt in the same TDR. No full reset, no `GRBM_SOFT_RESET`.
- A KIQ that does not answer (ring test of the KIQ fails): `Gfx->Failed` stays FALSE for the other rings, but node
  2 and node 0 stay closed. `Device->GpuStopUnconfirmed` is set, as in the retained resume, so the next stop does
  not trust the engines.
- After an unmap that passed and a map that failed: the queue is unmapped, which is a stopped queue. The proof
  of section 6 fails at the ring test, so the result is still a failure: the node stays closed.
- Bounds: KIQ ring test as at bring-up, HQD poll 100 ms, whole stage 2 per node under 300 ms (the `TdrDdiDelay` of
  5 s leaves room). Each attempt adds one hang record (`path`, a new verdict `QUEUE_RESET` and
  `QUEUE_RESET_FAILED <step>`), so the persisted record of the next boot names the step.

## 8. Tests

### Host model (CPU only, `driver/kmd/test/queue_reset_test.c`, in quick.ps1)

A model of the KIQ ring (records packets), the HQD registers under the select, safe mode and the ring tests, in
the style of `hang_recovery_test.c`. The production functions are extracted from gfx.c and bc250_gfx.c as the
other tests do.

1. Packet words: `UNMAP_QUEUES` and `MAP_QUEUES` match the amdgpu encoding for compute and GFX. The GFX reset
   packet is the 19 dwords of `reset_kgq` (write `CP_VMID_RESET` with `RESET_REQUEST` and `PIPE0_QUEUES`, the
   memory wait on the MQD offset of `cp_gfx_hqd_active`, the register wait).
2. Order: unmap, HQD poll, MQD, map, ring test. Never a map before a passed unmap. Select and safe mode are
   restored on every path.
3. Each step fails in turn (KIQ ring test, HQD stays active, map, queue ring test, wave proof with a wave of the
   VMID, wave capture error): the result is a failure, no later step runs, the gate stays closed, the hang record
   names the step.
4. Success: the node fence advances to the last submitted sequence, the aborted fence is the hung one, the VMID is
   reprogrammed, the gate opens, one hang record `QUEUE_RESET`.
5. Mode: with `HangRecoveryMode` 1 or 0 no stage-2 step runs (the red case for the old code: with mode 2 and a model
   in which the reset passes, `ResetEngine` returns success. Today's code returns failure).
6. S1: after a passed stage 2 the witnesses decide. A witness that reads busy keeps the failure.

### Lab (section 5.1 of the TDR plan: 3 min per trial, 4101 and no 1001, desktop back, fences move, a new device works)

| Trial | Node | Hang | Predicted |
|---|---|---|---|
| Q1 | 2 | `bc250hang --lab --long` compute spin | stage 1 drains (M938). Stage 2 not reached. Control. |
| Q2 | 2 | A7 call 10 `test_execute_indirect_multi_dispatch_root_descriptors` | stage 1 fails, stage 2 compute passes, 4101 |
| Q3 | 0 | a GFX `WAIT_REG_MEM` on a value that nothing writes, the Windows form of the M939 job. Lab flavour: `LabGfxHang` = delay in seconds (Parameters, read at `DriverEntry`, used once). The first WDDM job of node 0 after the delay waits behind the wait on gfx fence slot 12 (`gfx.c` `SubmitIbLocked`) | stage 2 GFX passes (as M944 on Linux), 4101 |
| Q4 | 0 | 3DMark SystemInfo scan on the candidate | stage 2 GFX after a no-retry CPG fault: not measured anywhere. Records the result. |
| Q5 | 1 | a paging job that SDMA0 cannot finish: lab flavour, `LabPagingHang` = delay in seconds (Parameters, read at `DriverEntry`, used once). The first paging job after the delay waits with an SDMA `POLL_REGMEM` on SDMA fence slot 6 for a value that nothing writes (`gfx.c` `GfxSubmitPagingAccess`) | `ResetEngine` refuses node 1, `ResetFromTimeout` resets SDMA0 (section 13), 4101 |
| Q6 | 0 | the Q3 hang with `LabQueueReset` 2: `ResetEngine` runs no stage 2, so the hang goes on to `ResetFromTimeout` | the `ResetFromTimeout` GFX queue reset, 4101 |

## 9. Code

| Part | File |
|---|---|
| The KIQ steps of 5.1 and 5.2 (`bc250_gfx_reset_kcq`, `bc250_gfx_reset_kgq`) | `driver/shim/bc250_gfx.c` |
| The fence slot write of 5.4 step 9 (`bc250_gfx_fence_write`) | `driver/shim/bc250_gfx.c` |
| GartLock, the gfx sequence, the wave proof, the VMID (`GfxQueueReset`) | `driver/kmd/gfx.c` |
| The pure decisions (`Bc250QueueResetWanted`, `Bc250QueueResetWaveProof`, `Bc250VmidForget`) | `driver/kmd/hang_recovery.h`, `queue_reset.h`, `vmid_pool.h` |
| The call after stage 1, the verdicts `QUEUE_RESET` (11) and `QUEUE_RESET_FAILED` (12) | `driver/kmd/wddm.c`, `hang_recovery.h` |
| The selector `LabQueueReset` (no effect while `BC250_QRESET_GFX_DEFAULT` is 1) | `driver/kmd/guard.c`, `entry.c` |
| The capability table by IP version (section 12) | `driver/kmd/queue_reset_caps.h` |
| The queue resets of `ResetFromTimeout` (section 13) | `driver/kmd/wddm.c` (`WddmAdapterQueueResets`), `adapter_reset.h` |
| The SDMA0 queue reset (`GfxPagingQueueReset`, `bc250_sdma_reset_retained_instance`) | `driver/kmd/gfx.c`, `driver/shim/bc250_sdma.c` |
| Host checks, with negative controls | `driver/shim/test/queue_reset.c`, `driver/shim/test/sdma_faults.c` (cases 7a to 7d), `driver/kmd/test/queue_reset_test.c`, `driver/kmd/test/adapter_reset_test.c` |

`CP_GFX_HQD_ACTIVE` and `CP_GFX_HQD_WPTR` are on the Gfx table of `gen_regs.py` as read-only witnesses. A failed
stage 2 writes `QUEUE_RESET_FAILED` with the failed step in `LastSeq`: the steps of `enum bc250_qreset_step`, 9 for
the wave proof, 10 for a register that the gfx sequence refused.

## 10. Open points

- Whether `CP_VMID_RESET` clears a CP stalled on a no-retry fault (Q4). If not, the next candidate is the
  fault itself (section 1, the CPG write to `0x1001C8000`): a write target the KMD did not map is a KMD defect of
  its own and is checked from the paging journal of that boot.
- Answered by Q2: `UNMAP_QUEUES RESET_QUEUES` does not end the CP-side wait of A7. The fallback of 5.3 does.
  Why the KIQ request leaves the HQD active is not known (`CP_HQD_DEQUEUE_REQUEST` and `CP_HQD_DEQUEUE_STATUS` read
  0 after it).
- Re-emission of the packets of other contexts on the same ring (amdgpu does it). Not needed for dxgkrnl, which
  resubmits. To be checked by the fence counters of Q2 to Q4.
- Node 0: a Windows client that makes a GFX CP-side wait (Q3) does not exist yet. The 3DMark fault of Q4 was fixed
  with the residency hold of tester.36, so it does not hang node 0 any more.
- The `ResetFromTimeout` half of section 4 is implemented (section 13). It is not measured on the lab yet.

## 11. Lab results (2026-11-04)

Evidence: [2026-11-04-queue-reset-q2](../../evidence/windows/2026-11-04-queue-reset-q2/README.md), fact M959.

| Trial | Result |
|---|---|
| Q1 | As predicted: stage 1 drains with one kill, event 4101, stage 2 not reached. |
| Q2 r1 (no fallback) | Stage 2 fails at step 3: the HQD stays active for 100 ms after the KIQ unmap. S1 refuses (busy `0x2A`), bugcheck 0x116. |
| Q2 r2 to r4 (fallback) | 3 of 3: `CP_HQD_DEQUEUE_REQUEST` 1 ends the HQD in 3 to 5 us. Then map, ring test and wave proof pass, with verdict `QUEUE_RESET` and event 4101. A later device works. About 165 ms per reset. |
| Q3, Q4 | Not run (section 10). |

The HQD witness shows the select of the right HQD (`CP_HQD_PQ_BASE` equals the ring base shifted by 8) and a CP
inside an indirect buffer (IB read pointer `0x56`).

## 12. Which device has which queue reset

The driver is to serve other AMD GPUs later, so the reset path does not assume the BC-250. `queue_reset_caps.h`
keeps two masks per IP version and runs only their intersection:

- What amdgpu declares (`AMDGPU_RESET_TYPE_PER_QUEUE` in the `sw_init` of each IP block, ref/linux-src 7d0a66e4b).
  GC 10.x: compute and GFX queues (gfx_v10_0.c:4955-4961). SDMA 5.0: 5.0.0, 5.0.2 and 5.0.5 with SDMA firmware 35 or
  newer (sdma_v5_0.c:1425-1433). SDMA 5.2: 5.2.0 to 5.2.4 with firmware 76 or newer, 5.2.5 with 34 or newer.
- What this driver implements. The shim has the KIQ resets of gfx_v10_0 for GC 10.1 and 10.3 and the SDMA register
  sequence of sdma_v5_0. SDMA 5.2 has another register layout that is not imported, so a 5.2 part has no SDMA
  queue reset until it is.

The device is found by its PCI id (configuration dword 0, as cumode.c reads it) in a table of known parts with their
IP versions. A device that is not in the table has no queue reset: `ResetEngine` and `ResetFromTimeout` then
behave as before tester.38. The WDDM initialization logs the result (`queue resets for PCI 1002:13FE: caps 0x7`).

One deviation from amdgpu, with its evidence: SDMA 5.0.1 (the BC-250) is not in amdgpu's list. Fact M943 measured
`sdma_v5_0_reset_queue` on SDMA0 of unit A (stop, soft reset and restore in 1.2 ms, later copies passed). The
capability is therefore a measured entry of the BC-250 row, not a rule for SDMA 5.0.1. The SDMA firmware version is
not read today (`BC250_SDMA_FW_UNKNOWN`), so no firmware-gated entry of another part passes until it is.

## 13. The queue resets of ResetFromTimeout (BD-180 point 3, BD-188)

Before tester.38, `ResetFromTimeout` ran the S1 wave kill and then the witnesses. A ring that the kill could not
drain (a CP-side wait, a CP stalled on a fault) and a paging packet that SDMA0 did not finish ended in the refusal,
then bugcheck 0x116. On 2026-10-19 (BD-188) that was the chain: `ResetEngine` refused twice, then
`ResetFromTimeout` logged "GPU memory access not proven stopped".

The order inside `WddmAdapterReset` is now:

1. Close every node, retire what arrived late, begin the reset on every node (unchanged).
2. Mode 2: the stage-1 wave kill on nodes 0 and 2 (unchanged).
3. Mode 2: `WddmAdapterQueueResets`. For each ring whose newest sequence did not retire, and only where the device
   has that reset (`Bc250AdapterQueueResetPlan`):
   - node 0: `GfxQueueReset` of the GFX queue for the VMID of the head job (5.2).
   - node 2: `GfxQueueReset` of the compute queue for the VMID of the head job (5.1, with the fallback of 5.3).
   - node 1: `GfxPagingQueueReset`. It runs `bc250_sdma_reset_retained_instance` on SDMA0 (stop, GRBM soft reset
     of SDMA0, check that it is stopped and frozen, clear the ring and its write-back slots, restart with
     `sdma_v5_0_gfx_resume_instance`), then one ring test, as `amdgpu_ring_test_helper` does after a restart.
     After both pass, the paging fence slot takes `PagingSubmitSeq`, and the paging drain completes the packet that
     the hardware had. SDMA1 is not touched: it carries no WDDM node.
4. The witnesses (unchanged): every ring retired, three register samples with GRBM, CP, CPC and SDMA0 idle. A queue
   reset that failed leaves its ring unretired, so the old refusal follows. A failed SDMA0 reset after its first
   register write sets `GpuStopUnconfirmed` (section 7).
5. The commit. A paging job that was queued and never ran no longer refuses the reset
   (`Bc250AdapterPagingQueueRelease`). The adapter-reset contract advances the last completed fence of every node to
   the last submitted one, and VidMm then purges all allocations from VRAM
   (timeout-detection-and-recovery.md), so the queued transfers are covered. Only the queue links are released. The
   payload belongs to the OS. A packet that is still on SDMA0 is a refusal, as before.

Deviations from amdgpu, each named in the code:

- `bc250_sdma_reset_retained_instance` went on to the pulse only for an engine that froze. `amdgpu_sdma_reset_engine`
  ignores the result of `stop_kernel_queue` and resets the engine anyway, because a hung engine does not freeze.
  The helper now follows amdgpu and keeps the stop result in the receipt (`stop_result`). The second quiescence
  after the pulse must still pass before any memory changes.
- amdgpu re-submits the jobs that the reset did not cancel. This driver re-submits nothing: the WDDM contract
  advances the fences instead.
- amdgpu serializes with its scheduler. Here every node is closed, `GartLock` holds off every other register
  sequence, and `Sdma0RingLock` covers the ring-test push.

Host checks: `queue_reset_test.c` (the caps table and its firmware gates, the node rules, the SDMA steps that leave
the engine unconfirmed), `adapter_reset_test.c` (the plan, the paging release), `sdma_faults.c` cases 7a to 7d (a
hung engine reaches the pulse, a hang that survives it changes no memory, and a refused safe-mode scope changes no
register). Each has negative controls in `tools/quality/quick.ps1`. The lab trials are Q3, Q5 and Q6 of section 8.
