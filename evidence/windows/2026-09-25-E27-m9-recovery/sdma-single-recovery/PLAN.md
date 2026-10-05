# Minimal real engine recovery: SDMA0

2026-09-25. Read-only design against current source (146 plus source-only paging
fixes M465/M467). No production changes, build, hardware access or deployment.

## Decision

Implement and measure one retained SDMA0 stop/reset/restore transaction first.
SDMA0 serves Windows copy/paging node 1, physical adapter EngineOrdinal 0.
Do not call full StartDevice, GfxStop/GfxStart, GpuSetPowerRetained or PSP reload.
Keep the same ring, writeback, fence pages, GART, GPU VA owners, firmware/TMR,
IH, compute engine and DCN scanout throughout.

The hardware basis exists: M388/M389 measured timeout-driven SDMA queue reset
on this unit, followed by fresh SDMA and compute content controls. Crucially,
the reset instance was **SDMA1**. This is a strong same-chip reference, not
already-proven SDMA0 recovery. The current comments claiming that the part
cannot reset anything are stale relative to M388; M53 covers the failed
whole-adapter Linux recovery/unload paths.

One necessary boundary: Microsoft explicitly says a ResetEngine that affects
a paging packet is followed by an adapter-wide reset. SDMA0 recovery alone
cannot make paging TDR recover successfully while ResetFromTimeout still
refuses. This plan establishes a real engine primitive; it does not close the
adapter recovery requirement or silently narrow M9 acceptance.

## Current implementation and why retained power is insufficient

| Current function | What it actually provides | Missing recovery property |
| --- | --- | --- |
| `driver/kmd/wddm.c:4054 Bc250WddmResetFromTimeout` | Closes health and both submission paths, returns failure | No hardware stop proof; truthful failure must remain until that exists |
| `wddm.c:4070 Bc250WddmRestartFromTimeout` | Closes health, logs, returns success | No restore after a future successful adapter reset |
| `wddm.c:4121 Bc250WddmResetEngine` | Returns NOT_SUPPORTED without changing fences | No engine reset or ready-empty-queue proof |
| `driver/shim/bc250_sdma.c:591 bc250_sdma_soft_reset_instance` | Original AMD assert/readback/50us/release/readback for one instance | Return 0 proves the access sequence ran, not that work can execute again |
| `bc250_sdma.c:620 bc250_sdma_quiesce_instance` | Queue disable, FREEZE/FROZEN or idle check, HALT, UTC L1 disable | Caller owns RLC scope and must restore the queue |
| `bc250_sdma.c:722 bc250_sdma_reset_for_reload` | Quiesces and resets BOTH instances, then leaves them stopped | Not live single-engine restoration |
| `bc250_sdma.c:315 bc250_sdma_gfx_resume_instance` | Fresh bring-up plus measured old-WPTR adoption | No explicit retained reset mode; must not blindly adopt a hung transport |
| `bc250_sdma.c:522 bc250_sdma_start` | Starts BOTH instances | Violates one-node reset scope |
| `driver/kmd/gfx.c:3237 GfxPowerRetainedLocked` | Restores retained owners after a previously idle power transition | Rejects failed/in-flight engines; halts CP and both SDMA, stops RLC, replays all stages |
| `wddm.c:1599 WddmSuspendRetained` | Requires no hardware, deferred or queued work; stops all timers/reports | A timeout has outstanding accepted work; clearing it to satisfy idle would fabricate retirement |
| `driver/kmd/power.c:26 GpuSetPowerRetained` | Coordinates SMU, WDDM, IH, GFX, PSP and GART suspend/resume | Much broader than an engine reset and based on different OS ownership |

Lines identify this read; function names are authoritative as edits continue.
M463 retained S4 content controls do not prove recovery of an executing/hung IB.

## Reference basis

PROVENANCE: AMD amdgpu SDMA source is MIT as recorded by the existing shim and
E29 source receipts; Linux tree is GPL-2.0, and the separately catalogued 2026
patch series is read-only reference. No upstream code was copied in this review.

- `ref/linux-src` v6.18, `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`:
  `drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c:688` (restore=true programs RPTR and
  WPTR from retained software WPTR), `:1330` (soft reset), `:1566` (stop),
  `:1615` (unfreeze and restore); `amdgpu_sdma.c:553` serializes each physical
  instance; `amdgpu_ring.c:794-827` stops scheduling, backs up commands, tests
  the restored ring, then handles guilty fences and re-emission.
- Exact 6.18.52 source retained under
  `scratch/m9/linux-6.18.52-source-check/sdma_v5_0.c`: same restore function at
  688, stop at 1558, restore at 1607. Source/recipe limits are in M385; do not
  equate a recipe match with proof of the running module's exact build.
- `bc250-win/evidence/linux/2026-09-24-E29-sdma-reset/RESULT.md` and
  `comparison/RESULT.md`: actual stop/reset/restore return 0, no full-reset
  probes, original job fails, new physical SDMA1 marker/fence passes, new
  compute control changes all 256 dwords correctly. The 56 decoded accesses
  show queue/cache disable, freeze, reset, unfreeze, ring/doorbell restore.
  There are no RLC_SAFE_MODE writes; zero clock-gating policy is already fixed
  in the current shim (M390). Do not reintroduce forced safe-mode requests.
- `ref/web-docs/sdma-reset-2026-09__WARN-GPL-newer-than-linux-src/series-thread.txt`:
  patch 3 at line 890 diagnoses duplicate execution on SDMA4.4.2, not a proven
  GFX10 defect; patches 5/6 at 1206/1255 clear old ring contents and CPU/WB
  pointers before restart. Their general transport-lifetime lesson applies:
  never keep a replayable old ring and also replay its commands elsewhere.
  This newer policy is not the measured E29 sequence. Preserve that distinction.
  Public thread: https://ratatoskr.run/amd-gfx/2026/09/17533121/t

## Windows ownership constraints

Local conceptual documentation is staging
`110f60eaf2ac5836e644d320c1e92c1011f2af5e`; DDI snapshot is
`7515063cea4c9e98db6a92986c5b4ddb0463fd16`, declarations from WDK 10.0.26100.

1. `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/`
   `nc-d3dkmddi-dxgkddi_resetengine.md`: PASSIVE_LEVEL, Level One; success
   requires completed reset, empty hardware queue and readiness for new work.
   Level One is not global exclusion. Other classes, level-zero calls and our
   own timer/DPC machinery need explicit synchronization.
2. `ref/windows-driver-docs/windows-driver-docs-pr/display/`
   `tdr-changes-in-windows-8.md`, sections "Packets unaffected", "Calling
   sequence", "Special cases": Windows snapshots submitted/completed fence
   IDs and suppresses timed-out-engine notifications at DIRQL, flushes queued
   DPCs, then calls ResetEngine. LastAbortedFenceId must identify a real queued
   packet or the genuinely last completed packet in the empty-queue race.
   Paging replay uses the original fence IDs/order; render replay gets new
   fence IDs. The scheduler, not a Linux-style KMD backup loop, owns replay.
3. The same document, calling sequence step 9, requires adapter reset when an
   affected packet is paging. Failure from ResetEngine also escalates. Do not
   wire a successful SDMA paging reset and claim that full TDR now works.
4. `ns-d3dkmddi-_dxgkarg_resetengine.md` and the WDK declaration contain
   NodeOrdinal, EngineOrdinal and LastAbortedFenceId. The conceptual article
   mentions a returned LastCompletedFenceId once; that is not an ABI member.
   Keep actual completion history separate from the aborted packet receipt;
   do not emit a fabricated DMA_COMPLETED notification for aborted data.
5. `nc-d3dkmddi-dxgkddi_resetfromtimeout.md` plus
   `display/thread-synchronization-and-tdr.md`: successful adapter reset must
   leave GPU memory reads/writes stopped while Windows evicts/unmaps resources.
   ISR/DPC can still run. RestartFromTimeout signals that cleanup finished.
   The retained-S4 assumption that every OS mapping survives does NOT apply.

Primary public contracts:
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/tdr-changes-in-windows-8
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_resetengine
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/thread-synchronization-and-tdr
These were read locally; no online lookup was necessary.

## Minimal implementation split

### A. Hardware primitive, no DDI success yet

Add an explicit retained single-instance restore path in the shim, using the
original AMD restore=true sequence. Proposed internal API:
`bc250_sdma_reset_retained_instance(adev, instance, receipt)`.
Receipt records selected instance, prior software WPTR, freeze/reset readbacks,
restored hardware/WB pointers and the first failed stage. It is not permission
to retire OS resources. Keep bounded polls and propagate Sequence.Fault.

The caller supplies exclusive ownership of that ring and retained backing.
Sequence: source-derived RLC scope -> stop selected queue and verify quiescence
-> leave scope -> selected soft reset -> discard stale transport while halted
-> source-derived scope -> unfreeze -> restore selected ring -> leave scope.
Check stop/restore results even though older Linux common reset code ignores
their return values. A failed freeze/idle predicate is not permission to pulse
reset anyway. No PSP commands, firmware reads or shared-GART reset are needed.

For the first source-matched version use the retained software WPTR as the empty
boundary, with hardware RPTR=WPTR, matching AMD restore=true and E29. Clear the
old ring to NOPs while stopped and synchronize the selected CPU/WB RPTR/WPTR to
that boundary before enabling it. Preserve monotonic private fence sequence
identity. This combines the measured restore sequence with explicit stale-ring
discard; label the composition as new Windows policy requiring its own test.
Do not use the fresh-start zero/write/readback/adopt branch for recovery.
Do not clear another instance's WB or any OS allocation/fence.

After restoration run a driver-owned marker/fence probe on this same ring,
with a fresh poison value and fresh sequence. The current
`bc250_sdma_ring_test_submit/wait` are useful hardware oracles; new nonzero
transport positions must pass their capacity checks. Empty/ready is accepted
only after that probe really completes and no packet remains on the ring.
Reset-bit readback alone never sets Ready.

### B. Narrow KMD coordinator

Introduce a copy-node recovery gate/epoch synchronized with `wddm->Lock` and a
PASSIVE lifecycle owner. It must exclude stop/power, diagnostic submissions,
the node-1 timer and IH-driven `WddmGpuFencePaging`, including publishers already
past their first admission check. Join CPU publishers/readers, not the hung
hardware fence. Never hold a spinlock across waits/MMIO polling. Preserve the
existing GfxPagingLock-before-GartLock order for the shim backend/owner scope.
Do not use global `Stopping` or destroy all IH delivery for a one-node reset.

Because `GfxAccessClose` covers compute too and existing paging failure is
sticky for the entire device start, implement a specific recovering state and
generation check rather than blindly clearing `PagingSubmitFailed`. A late old
DPC must not reopen the gate, publish completion or clear a new in-flight claim.
Only the successful reset+fresh-content receipt permits the selected fault
state to reopen. Adapter/global errors and other-node failure remain closed.

Keep `PagingHwFence`, `PagingHwSeq`, submitted/completed OS histories and the
accepted software queue until hardware discard is proved. For a future DDI
connection, choose LastAbortedFenceId from the actual active OS job, not the
internal probe sequence or the tail that never reached hardware. Detach
unexecuted software descriptors only as scheduler-owned replay candidates;
retain ownership until the DDI outcome gives Windows authority to replay/free.
Never replay both here and from SubmitCommand's Resubmission path. M465's
preemption tests provide useful queue-ownership controls, but do not establish
reset semantics by themselves.

Keep the self-only dependent-node mask contingent on a measured reset that does
not disturb CP/compute or shared translation. The mask cannot be justified only
by having two rings. No adapter-wide success path is part of this small change.

## First measurable step

First deliver A with actual-source host replay and a bounded, typed internal
self-test callable **before WDDM publication**, after normal GPU startup has
prepared persistent owners. No new unrestricted MMIO escape is needed. The
driver owns all work at this point; Windows has no accepted paging packet to
misclassify. Leave ResetEngine returning failure during this first experiment.

The first hardware test on SDMA0 is idle reset -> fresh marker/fence and full
copy readback using the same retained addresses. This is a real reset test,
not yet a timeout test. Then a separate bounded test can submit the existing
E29-style memory-poll IB, demonstrate it pending, reset it without waiting for
completion, and verify a new marker/copy on the same ring. The stale job's marker
must remain poisoned after releasing its old poll condition. Do not report the
interrupted diagnostic job as completed. Keep all its memory pinned throughout
observation and recovery. No OS-owned paging hang injection before the broader
adapter-reset path is ready.

Host acceptance: original per-instance restore register order; instance 0 vs 1
write isolation; preserved reserved bits and native cg_flags policy; old ring
content unavailable after reset; ring/WB pointers agree; same owner addresses;
failed freeze or failed fresh fence keeps admission closed. Remove the reset
or restore operation in a mutation and require that its corresponding replay
or content-state assertion fails. A host model is not hardware reset evidence.

Lab acceptance: freeze/reset/restore stage receipts with named AMD registers,
old/present ring and WB identities, fresh fence and byte-level copy oracle,
old-marker nonexecution after release, and post-reset CP shader content control.
Preserve baseline controls before reset and trace source/binary hashes. Measure
SDMA0 explicitly; Linux's SDMA1 result alone is insufficient. No full reload,
OS reboot or firmware reload between these controls. A failed experiment retains
owners and follows the existing recovery policy; it must not silently return
STATUS_SUCCESS. This review performed none of these lab actions.

## What stays open after that first pass

OS-directed ResetEngine queue/fence reconciliation and actual TDR acceptance;
adapter-wide no-memory-access ResetFromTimeout and post-cleanup restoration;
hang classes that do not reach AMD's freeze/idle predicate; repeated reliability.
The smallest useful next commit is the single-instance restore primitive and
its source-matched host tests, followed by one controlled SDMA0 hardware test.

## Source identity at review

- bc250-win\driver\kmd\wddm.c SHA256 EA0ED76FDE6C44481E473E9198669820186FCC3A7000D1CA7D2B80929B608E5F
- bc250-win\driver\kmd\gfx.c SHA256 02BB3C246C3495059B8E5D4F3D6123841EFA46A1645180B3FBC9AC18E28BE4E7
- bc250-win\driver\kmd\power.c SHA256 D2FFED418217B9661C4A525EE807968867828CEA3434DCEA6A2F4375542CED6F
- bc250-win\driver\shim\bc250_sdma.c SHA256 FB1A82AB4389866718F35FD4AC654696ACDDAD3B3E40A753D25143F544869520
- ref\linux-src\drivers\gpu\drm\amd\amdgpu\sdma_v5_0.c SHA256 D38D98E12E323CE658E41D47EA1E4B617FAB07D5037548F945054845FAC68CEC
- scratch\m9\linux-6.18.52-source-check\sdma_v5_0.c SHA256 C1C9BDB4DBC63442917586A3C8A1E96520FC9950E920848BBD4DBC248AA6CE82
