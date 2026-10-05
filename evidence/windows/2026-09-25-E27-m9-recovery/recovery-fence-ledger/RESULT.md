# Recovery fence ledger prerequisite

2026-09-25. Source/host/build only. No lab access, deployment, driver version change or reset-success wiring.

## Changes

Production: `driver/kmd/wddm.c` only. The task-local `wddm.patch` is relative to the dirty source at task entry; unrelated existing work was preserved.

- Add one per-node `BC250_WDDM_FENCE_LEDGER`: owner-local epoch, last OS-submitted fence, last hardware-executed fence and separate validity bits. Existing LastReportedFence/LastReportedValid remain the sole notification/rejection history; no duplicate writer is added.
- Actual SubmitCommand and SubmitCommandVirtual wrappers record a successful DDI's OS-owned fence under wddm->Lock, before decrementing ActiveSubmissions. This includes a valid nonempty packet that the implementation could not dispatch but retains for recovery while returning STATUS_SUCCESS. Such a packet is not excluded from adapter-reset ownership merely because RefusalPending is set. Conversely STATUS_INVALID_PARAMETER from a virtual submission does not advance Submitted; existing RejectedPending ordering is unchanged.
- Actual WddmGpuFence records HwFence only after GfxFenceArrived, before DeferredFence can replace the completion-notification ID. WddmGpuFencePaging records PagingHwFence only after its hardware fence arrives. CPU/empty completions, watchdog expiration, rejected packet bookkeeping and a later queued packet do not manufacture hardware execution.
- Signed fence deltas handle wrap and avoid regressing the accepted high-water mark when VidSch replays paging work with original IDs. Zero is a valid fence, independently represented by the validity bits. This follows the same bounded signed-distance convention as the existing fence/refusal code, not arbitrary numeric max.
- Hardware-pending state and the borrowed paging queue slot carry an epoch. A stale queued job, stale hardware observation or stale returning submission cannot update a newer ledger. The added paging slot remains within the existing 64-byte private reservation, checked by the actual C_ASSERT; no private ABI size/version changes.
- A locked snapshot copies the ledger and existing notification history only when ActiveSubmissions on that node is zero. This covers the valid race where hardware completes before its submitting DDI finishes. ResetEngine currently uses it only to log diagnostics; it still returns STATUS_NOT_SUPPORTED and never writes LastAbortedFenceId.

The epoch starts at1 for each allocated WDDM owner and survives retained S4. It is not a new global adapter identity and no live path advances it yet. PnP destruction continues to join DPCs before freeing the old owner. Future recovery must close and join CPU publishers/observers, settle old queue ownership, then advance/reset the relevant epoch/history. The new checks supplement that transaction; they do not replace lifecycle exclusion, implement reset, or make every pre-existing notification/watchdog field independently epoch-aware.

## Contracts and meaning

Local conceptual snapshot staging110f60eaf2ac5836e644d320c1e92c1011f2af5e; DDI references/WDK10.0.26100.

- `ref/windows-driver-docs/windows-driver-docs-pr/display/tdr-changes-in-windows-8.md:63-100`: original paging fence/order on replay; ResetEngine aborted fence must name actual queued or completed work; adapter reset history advances to last submitted. This ledger keeps that OS-owned submitted history separate from actual execution.
- `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual.md`: invalid private submission has a real rejecting status, distinct from the physical submission path's required success/fault policy. Existing routing and return values were not changed.
- `.../ns-d3dkmddi-_dxgkarg_resetengine.md`: only LastAbortedFenceId is an output; the snapshot is diagnostic groundwork, not an automatic reset receipt.

`Hardware` is deliberately hardware execution only. It is not by itself permission to return that fence as LastAbortedFenceId: future recovery must also evaluate the active queue, completed CPU/empty packets, scheduler snapshot and the reset's actual affected work. In particular existing LastReportedFence may include legitimate deferred software completion or rejection history; neither can be silently substituted for an observed hardware fence.

## Actual-source tests

New durable fixtures:
- `experiments/E27-m9-inference/generate-recovery-fence-ledger-test.py`
- `experiments/E27-m9-inference/recovery-fence-ledger-test.c`

The fixture composes existing real paging parser/queue/preemption code, then extracts the actual ledger/snapshot functions, GFX fence/publication functions and both public submission wrappers. OS/GFX interactions are mocks. The inner DDI implementation is a controlled admission-result mock, so the test establishes wrapper ownership semantics; it does not independently revalidate every malformed private-data parser.

Existing queue harness adaptations are limited to importing the new actual ledger type/helper and adding its mock state fields:
- `generate-paging-queue-test.py`
- `paging-queue-test-prefix.c`

Results:
- `/W4 /WX` host build: **1199 checks, 0 failures**, `positive.log` and `positive.c`.
- Existing M465 preemption harness: **1074 checks, 0 failures**, `preemption.log`.
- Actual A/B/C queue: C is last OS-owned, only A is hardware-executed; delayed notification stays separate; empty-queue race retains A; original-ID B/C replay does not regress Submitted and executes each exactly once.
- Rejected virtual packet does not enter Submitted; its existing rejected-notification history can advance without changing Hardware. Physical success with hardware refusal does enter Submitted and remains RefusalPending, with no fake execution/completion.
- Compute hardware fence121 remains121 when deferred software notification becomes122. Ring refusal advances only OS ownership.
- Stale returning wrapper, stale compute/paging observations and old queued epoch leave the new execution history untouched; ownership remains retained for the future recovery coordinator. Zero/wrap/original-ID ordering and busy snapshot refusal pass.

Negative controls compile and fail targeted assertions:
- `--accept-rejected`: 1199 checks, **1 failure**.
- `--hardware-is-reported`: 1199 checks, **3 failures**.
- `--ignore-epoch`: 1199 checks, **2 failures**.

These are failure-discriminating tests of the actual changed functions, not hardware recovery acceptance. The parent already has M465's independent normal queue/report coverage; that behavior remains exercised by the composed harness.

## Frozen build

Final source snapshot: `source-final/`, **483 files**, all SHA256SUMS entries verified by verify.py. Earlier `source/` and wdk.log retain the first build's C4701 diagnostic-snapshot initialization warning; this was corrected with a zero initializer. No warnings were suppressed.

Final full unsigned WDK build passes (`wdk-final.log`). SYS:

`scratch/build/recovery-fence-ledger-final/package/bc250kmd.sys`

SHA256 **63BDF541D6F628880F43C9B35342003E7DA56CE6BD03F9A75A740D8A359A5E38**; Authenticode **NotSigned**. No catalog/certificate/signing/deployment steps ran. Stack check:568 unwind functions, largest3992-byte frame in existing GfxPagingBuildUpdate; only the pre-existing stack warnings appear.

Current/frozen `wddm.c` SHA256 **62574FEA71C16688E5F8502A9107E91C0B2D4D086EA7789B453A55AA518B3B1F** (file bytes; verify.py additionally compares normalized text). The snapshot does not change driver version147.

## Remaining scope

No recovery gate, hardware reset invocation, accepted-work discard, cleanup branch, OS replay integration, adapter restart or health reopening was added. ResetFromTimeout still fails honestly and ResetEngine still refuses. The plan at `scratch/m9/recovery147-plan.md` remains the integration guide. Lab/S4/content results obtained by the coordinator use the unchanged installed147, not this source-only prerequisite.
