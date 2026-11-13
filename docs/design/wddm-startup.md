# WDDM startup and paging readiness

Current acceptance: [M9 acceptance status](../archive/research/m9-acceptance-status.md), reviewed through M462. Historical results below do not prove current candidate acceptance.

Status: implementation plan, 2026-09-23. Source review:
[facts M232](../facts/kmd.md#m232) and
[immutable source snapshots](../../evidence/windows/2026-09-23-E27-m9-inference/startup-contract-review/).
This covers a remaining M9 DMA contract requirement, not a new milestone.

## Required outcome

When the OS can call a supported paging operation or submit its commands, the
driver must already own the resources and initialized engines needed to execute
it. A later CLI RUN cannot be the readiness mechanism for production WDDM.
Display-only mode remains a separate startup path.

At the original M232 review, the manual sequence opened full WDDM before GART,
PSP, IH and GFX initialization. Shared automatic startup subsequently replaced
that dependency. M317 observes both engines ready before initial OS paging on 07102.
The remaining acceptance gates below include repeated initialization and
resource lifetime, not a continued dependency on manual CLI RUN.

## Initialization and ownership

| Phase | Required work | Publication and failure obligation |
|---|---|---|
| Prepare | Identify VRAM. Allocate subsystem state, CPU mappings, paging metadata and private staging. Check gates/resources. | No supported paging capability may depend on a missing resource. Retain explicit ownership for each successful allocation. |
| Firmware preparation | Use existing psp.c file loading/validation at PASSIVE_LEVEL before GartLock. | Fail before hardware writes when files or storage are unavailable. Reuse existing validation. Do not introduce another firmware uploader. |
| GART | Derive geometry through AMD setup, initialize table, enable translation. | The M229 descriptor and future builders use the same captured geometry. No later destructive table reset may remove accepted OS mappings. |
| PSP and interrupts | Load firmware through existing PSP sequence, initialize IH and its ownership. | Preserve the actual ring/TMR/interrupt state reached even on partial failure. Do not infer quiescence from a software state flag. |
| Engines | Run required initialization and ring controls. Establish paging ring, marker, stage and invalidation-engine ownership. | Publish readiness only after success. A failed engine start requires proven halt or retained/quarantined DMA resources. |
| WDDM operation admission | Publish adapter state and the supported capabilities/segments consistent with initialized resources. | Earliest paging calls use a working implementation. No empty success, infinite buffer retry or dependence on a later CLI command. |
| Stop/restart | Stop admission, join builders/submissions, stop hardware, then release or retain resources according to actual retirement. | Never unload PSP or free mapped pages merely because a timeout expired. Restore mappings only within a verified new device/engine generation. |

The exact PnP/interrupt callback ordering must be verified before enabling this
sequence automatically. Moving RUN inside StartDevice invalidates the current
failure-cleanup comment that no RUN has occurred. Changing that comment alone is
not a fix.

## Start without a firmware display

Status: BD-341, 2026-11-09. Stage A is implemented. Stages B and C are not
implemented.

The firmware lights a display only when it finds a monitor at POST. When it
finds no monitor, `DxgkCbAcquirePostDisplayOwnership` fails, or it returns a
geometry with a zero width, height, pitch or address. Before BD-341 the start
failed with `STATUS_GRAPHICS_NO_VIDEO_MEMORY` (Code 43), and a monitor that
arrived later never got a picture. This behaviour does not depend on the board.

The start contract is now:

| Item | Firmware display present | No firmware display |
|---|---|---|
| `Device->Post` | The answer of Windows. | All zero. Nothing of a partial answer is kept. |
| `Device->DisplayStart` (`display_start.h`) | `BC250_DISPLAY_START_INHERITED`. | `BC250_DISPLAY_START_NO_POST`, for the whole start. |
| Start record | `DisplayStart` 1. | `DisplayStart` 2, also when the start succeeds. |
| Framebuffer map, boot logo, sample | Done. | No map. The logo and the sample find no framebuffer and log that. |
| Start blank and final blank | Done on OTG0. | Not done. The driver reads the pipe and `D1VGA_CONTROL` once and logs them (`display cold:` lines). These are reads only, and only through the BAR5 mapping that the start made by its own gate. |
| Inherited timing, mode list, DP audio | Done. | Not done. `InheritedSignalValid` stays FALSE, and each VidPN path that needs it refuses. |
| VRAM layout | Above the firmware framebuffer (`VramFramebufferExtent`). | Above the range at offset 0 that the firmware can still scan out of, with the size that Linux gives (see below). |
| Connector | Inherited or programmable route. | No HPD arm and no register write (`connector: start without a firmware display`). |
| Child reports | As before. | `HpdAwarenessPolled`, and every `QueryChildStatus` answer is "not connected" (see below). |
| OS visibility requests | Reach OTG0. | Touch no register, unless a runtime mode was committed. |
| D0 (`PowerRestoreDisplay`) | Compares the new POST answer with `Post`. | Compares nothing and restores nothing. A display that the firmware lit in D3 goes black at once (see below). |
| Stop (`DcnStop`) | Restores the firmware surface and shape. | Restores nothing, unless a runtime mode was committed. |
| Cursor off at stop, release and bugcheck | Written. | Not written, unless a pipe of this start had the cursor. |
| PnP release | Hands back the firmware or runtime mode. | `STATUS_NOT_SUPPORTED` with zeros, unless a runtime mode is on screen. |

The VRAM reservation is the one of Linux on this part. `gmc_v10_0_get_vbios_fb_size`
reads `D1VGA_CONTROL` through the DCN 2.0.0 header. When `D1VGA_MODE_ENABLE` is
set, the size is `AMDGPU_VBIOS_VGA_ALLOCATION` (9 MiB). Otherwise it is the
HUBP0 viewport height times the pitch times 4, also when the pipe is off.
`amdgpu_gmc_get_vbios_allocations` then sets the size to 0 when less than
8 MiB of VRAM would remain. Thus "no pipe lit" does not mean "0 bytes": an off
pipe can keep its old HUBP0 values. The start reads the three registers once
(`ModesetReadColdPipe`) and keeps the result in `ColdScanoutBytes`.
`VramFramebufferExtent` applies the 8 MiB rule. When the start cannot read the
registers (no BAR5 mapping or a refused read), it keeps 9 MiB. The connector
buffer and its zero fill, and the VidMm segment, start above this range.
The GART window, the PSP window and the GPU memory pool at the top of VRAM
use the same range (`VramFirmwareRangeBelowTop`). When the carve-out is
known and the range reaches into the reserved tail at the top
(`BC250_VRAM_TOP_RESERVED`), the start fails in the step `ColdScanoutReserve`
with the line `vram: cold scan-out reserve`. A stale HUBP0 pipe on a small
carve-out can give such a range, and the firmware could then scan out of the
memory of those owners.

The child report says what stage A delivers. Nothing is armed, so no
indication comes, and the child is `HpdAwarenessPolled`: dxgkrnl learns the
state only from `QueryChildStatus`. Stage A answers "not connected" every
time, because it cannot light a pipe that the firmware did not light. A
"connected" answer would let Windows commit a path that stays black. Stage C
makes the poll answer the HPD sense, and it reports
`HpdAwarenessInterruptible` only when HPD is armed (`Bc250ChildHpd` in
`display_start.h`).

The firmware can light a pipe while the GPU is in D3. Examples are a resume
from hibernation, and a fast startup with the monitor now on. That pipe scans
out of the bottom of VRAM, and the memory above the reservation of the start
belongs to the connector buffer and VidMm. When the D0 POST answer has a
display, `PowerHoldFirmwarePipe` blanks OTG0 at once with the start blank
(black data, sync kept), before an engine resumes. It logs `power: D0 found a
firmware display` and counts the event (`firmware pipes held` in the child
summary). The start does not take the pipe over: that would change the
geometry under the OS handles. A WDDM miniport cannot ask for a stop and start
of the adapter. After `IoInvalidateDeviceState`, the PnP manager gives the device stack
`IRP_MN_QUERY_PNP_DEVICE_STATE`, but dxgkrnl owns the PnP IRPs and has no DDI
that lets the miniport set `PNP_DEVICE_RESOURCE_REQUIREMENTS_CHANGED`. Thus the
monitor stays black until the next start of the adapter, which keeps the
pipe. With fast startup, that is the next restart, not the next shutdown.
Stage C makes this case normal: it lights the pipe as a runtime mode.

No reader of the POST geometry uses a zero geometry. The readers
that need a real display are on the inherited path, which this start does not
run. The other readers (scan line, DirectFlip handshake, flip pitch) refuse a
zero size or are reached only after a committed VidPN.

What stage A gives: the adapter starts, with no Code 43. GPU compute, the WDDM
memory manager and the start record work as on a start with a display. What
stage A does not give: a desktop on a monitor that arrives later. The driver
has no cold mode set, because it programs only a pipe that the firmware lit.
Two more stages are necessary:

- Stage B: the cold DCN state.
- Stage C: the connector without an inherited pipe.

[The cold-modeset plan](cold-modeset.md) gives the steps, the AMD functions
to port and the lab evidence that stage B needs. Every part of the driver
reads the one display-start state, so stages B and C add code paths for
`BC250_DISPLAY_START_NO_POST` and change no reader of the geometry.

Part 1 of BD-341 (a monitor that is on at the restart, and Windows has no
monitor) has no proven cause yet. A hotplug worker that disables the link,
indicates "disconnected" and then fails discovery leaves Windows with no
monitor. To make this visible after the log ring wraps, the connector keeps the
first eight child events of a start (relations, status answers, indications,
worker ends) and the latest one. The start clears these events at its top
(`ConnectorChildBegin`), so that the first child event of a start is kept with
the time base of that start. `WddmSummary` and the connector stop print
them again with the counts and the live HPD0 sense
(`connector: child summary` and `connector: child kept` lines).

## Successful-start confirmation

The remaining full-table monitor handshake is specified in
[successful-start confirmation](wddm-start-confirmation.md). It requires a fresh
device-start identity and completed presentation progress before checked durable
confirmation. This is an implementation contract, not current acceptance.

## Implementation work

1. Extract internal initialization entry points shared with diagnostic escapes.
   Avoid a second copy of GART/PSP/IH/GFX sequences. Preserve lock order and keep
   firmware file I/O outside the fast mutex.
2. Represent completed and partially completed startup phases explicitly, with
   a reverse ownership/unwind path. Inject failures at every phase in host tests.
   A transition test must inspect resource retention, not just returned status.
3. Integrate startup/admission with the WDDM device lifecycle, then check earliest
   callback delivery. Queue resources must exist before first execution. Any
   deferred work must retain ownership and completion obligations.
4. Remove the production dependency on post-start CLI initialization. Keep CLI
   staging for controlled diagnostics, with repeated RUN/ENABLE rejected while
   OS resources are live unless a verified recovery transition owns them.
5. Check a cold start, repeated device start, partial startup failure and OS
   map/transfer/unmap lifetime on the lab. These gates remain incomplete until
   actual engine completions, intact mappings and cleanup are observed.

## Rejected shortcuts

A CPU bootstrap PTE write alone is insufficient: current GART ENABLE zeros the
whole table afterwards. Recording desired mappings for replay may help recovery
or physical-segment resolution, but it does not make an unavailable engine ready
and cannot justify reporting an unexecuted DMA command complete.

STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER means capacity exhaustion. It must not
be used as a readiness wait. Internal DEVICE_NOT_READY/INVALID_PARAMETER returns
in current builders expose unresolved cases. They are not a completed OS-facing
failure policy.

## Acceptance evidence still required

- Failure injection proves no early publication, double release or loss of
  partially initialized ownership. Successful control reaches real admission.
- Live traces establish startup ordering and first OS paging/submission callbacks.
- Known-pattern map/transfer/unmap controls prove the right physical pages, cache
  visibility, real fence completion and preservation across every supported
  initialization transition.
- Hardware halt/recovery evidence supports freeing DMA storage. Failed recovery
  remains a failure. Elapsed time and a DWM restart are not halt evidence.
- Full M9 still includes alias handling, restricted DDI returns, CPU aperture
  policy, OS lifetime, paging pressure and equivalent Windows/Linux performance.


## Milestone record M233 to M462

The progress record of the finished milestones M233 to M462 is in [the archive](../archive/design/wddm-startup-progress-2026-09.md). It is a record, not design text.
