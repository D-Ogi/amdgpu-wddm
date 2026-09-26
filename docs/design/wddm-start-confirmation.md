# Full-WDDM successful-start confirmation

Status: implemented in145; one warmPnP automatic-confirmation control passes
(M457,2026-09-25). See [runtime evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/automatic-confirmation145/RESULT.md).
The144 kernel already required a checked durable admission increment;145 adds
the full-table confirmation handshake below. Broader lifecycle coverage remains
open; this is not full M9 acceptance.
This work belongs to the existing M9 startup requirement.

## Positive path

1. StartDevice creates a fresh device-start identity. It must change on PnP
   restart and driver reload, even when UnconfirmedStarts has the same value.
   A wall-clock timestamp alone or the registry counter is not an identity.
2. Successful automatic engine initialization publishes readiness for that
   identity. Completed hardware presentation publishes a separate witness.
3. The interactive monitor samples a typed health reply every five seconds.
   For at least 60 seconds it observes the same ready identity, visible active
   output, and fresh successful presentation progress. A missing or unhealthy
   sample, counter regression or identity change restarts the interval.
4. The monitor requests confirmation of that exact identity. The kernel checks
   it against the live identity and current readiness, then writes zero and
   checks the registry flush result. Only that success means confirmed.

A monitor process, successful StartDevice, stage50, programmed flip count or
hardware vsync tick by itself does not prove a completed presentation. A fresh
query of a stale counter is also insufficient. This confirms startup progress;
it does not prove pixel correctness, GPU rendering, model correctness or M9.

## Presentation witness and lifecycle

Use the existing WddmDcnVsync completed-primary branch, after
WddmReadCompletedPrimary accepts a stable primary sequence and hardware latch.
Do not count the fallback that reports the previous buffer. Count distinct
completed primary programming sequences, not repeated reports of one address.
PrimarySequence alone is insufficient: it advances on rejected and unchanged
requests too. Publish a separate witness only after a changed hardware flip
succeeds, then consume it through the stable completed-primary path.
Keep an independent last-success monotonic time so a stalled screen cannot
pass by retaining a previously nonzero count. A static desktop may defer
confirmation; the active overlay provides regular presentation during startup.

Close health admission before stop, adapter power-down and TDR handling; D0
alone must not reopen it while full resume is unsupported. Visibility/mode
changes invalidate the monitor's observation interval. Readiness must include
both execution and paging admission, not merely Started. A query must never
follow a Wddm/Gfx pointer whose owner can be freed concurrently.

Keep the query reply in adapter-owned storage with explicit synchronization.
The implementation must prove writer ordering and reader lifetime, including
StopDevice racing an escape; do not reuse GET_INFO as a lifetime guarantee.
No polling query may read BAR registers, submit work or idle GPU scheduling.

## Typed interface

Use one versioned structure with exact byte-size validation, operation status,
current device-start identity, readiness/visibility flags, completed-primary
count, last completion age, and confirmed identity/status. Confirm includes the
expected identity. Unknown versions and absent support leave the counter alone.
Keep explicit human confirmation separately labelled; it does not manufacture
an automatic-health result. Preserve the existing display-only policy.

The read path should avoid HardwareAccess. NoAdapterSynchronization does not
make shared data safe: prove adapter-owned snapshot synchronization explicitly.
The one-time confirm path may use HardwareAccess=1 with all other escape flags
zero, recheck the live identity/readiness and perform checked persistence at
PASSIVE_LEVEL. Do not hold a spin lock across registry I/O. Review the exact
lifecycle exclusion at implementation time, including ISR/DPC writers.

Local Microsoft sources reviewed:

- windows-driver-docs/windows-driver-docs-pr/display/threading-and-synchronization-second-level.md
  (staging 110f60ea): HardwareAccess uses Level Two and idles graphics/DMA work.
- windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/nc-d3dkmddi-dxgkddi_escape.md:
  DxgkDdiEscape runs at PASSIVE_LEVEL.
- windows-driver-docs-ddi/wdk-ddi-src/content/d3dukmdt/ns-d3dukmdt-_d3dddi_escapeflags.md:
  HardwareAccess semantics; NoAdapterSynchronization has no explanatory text.

These sources support the synchronization constraints, not the proposed private
health ABI. Its lifetime proof and tests remain implementation obligations.

## Implementation synchronization and scope

The adapter owns a lifecycle mutex, a short snapshot spin lock and read rundown.
Queries copy only this storage. PASSIVE_LEVEL lifecycle transitions serialize
with confirmation; DPC engine faults immediately close the snapshot and advance
its epoch under the spin lock. Registry I/O never holds that spin lock.

The confirmation decision linearizes at the live healthy identity check before
registry I/O. It certifies the preceding startup interval, not future operation.
If an asynchronous fault advances the epoch during the flush, the old interval
may already be durably acknowledged, but the reply must not certify the new
epoch. Do not restore the counter after such a race: that would manufacture a
new unconfirmed start. The monitor treats the changed/unhealthy reply as such.

The start identity uses boot-monotonic QPC plus a strictly increasing CAS value
within the loaded image. The reload assumption is explicit: completing a real
driver unload/reload takes more than a QPC tick. The monitor cannot carry an
observation interval across an OS restart. This is not a wall-clock timestamp
or the possibly repeated start-budget value.

## Acceptance

Host tests must exercise the actual confirmation policy: healthy progress for
60 seconds; no progress; repeated old-buffer reports; stale/missing samples;
new identity with the same registry counter; stop or power-down immediately
before confirm; write failure; and flush failure. A success before a checked
flush must fail a negative-control test.

On the lab, leave a genuine newly admitted start at counter1 and observe the
monitor change it to zero only after the required interval. Record generation,
completion progress and checked persistence status, plus retained OS/DWM
identity and the owner's input/display result. Do not manufacture a fresh start
by changing only the registry counter. No repeated OS restart is needed to
validate the first warm PnP path; AC-cold and power-resume tests remain separate.
