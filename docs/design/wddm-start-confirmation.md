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
The confirm path rechecks the live identity/readiness and performs checked
persistence at PASSIVE_LEVEL. Do not hold a spin lock across registry I/O.
Review the exact lifecycle exclusion at implementation time, including ISR/DPC
writers.

Confirmation flags (0.7.213): both operations take NoAdapterSynchronization=1
with every other escape flag zero. The confirm path used HardwareAccess=1 up to
0.7.212, which is a Level Two escape: dxgkrnl suspends the GPU scheduler for up
to one VSync while the handler holds the lifecycle mutex across a registry
flush, and the installer's logon task retries confirmation every 5.5 s for up
to two minutes. The handler touches only the start-health snapshot under its own
spin lock, mutex and rundown protection, then the registry - no register, no
mailbox, no state a stop frees - and the CU mode and DPM confirmations it calls
have always run from escapes that take NoAdapterSynchronization alone. For one
release the confirm path also admits the old HardwareAccess word, so a CLI, DLL
or overlay of 0.7.212 or older still confirms a start. Every other flag word is
refused before anything is read or written; the table of admitted and refused
words, per operation, is in driver/kmd/test/start_health_test.c and
tools/win/bc250kmd_cli/test_escape_flags.py.

The other direction is the client's business, and Bc250StartHealth carries it: a
driver of 0.7.212 or older refuses the new word at its gate (Status REFUSED,
NtStatus STATUS_INVALID_PARAMETER, Version written before the refusal), so the
export sends the same request once more with the old word and keeps sending it
that way for the rest of the process. That pair is a normal state of a release
upgrade - the installer copies the tools and defers the device restart, so this
release's logon task, Recovery action and overlay all run against the loaded old
driver until the next start. Without the retry every confirmation would be
refused for the whole 120 s of the logon task, which falls back to the boot-loop
guard alone and leaves the CU-mode and DPM requests unconfirmed: the next start
would come up at 24 CU and the floor clock.

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

## Runtime restart in a confirmed boot (BD-090, 0.7.216.3)

A runtime restart (`pnputil /restart-device`, a live driver update) starts the
driver again in the same boot. When nothing confirms the new start, each such
start adds one count, and the second one is refused with Code 43. The kernel
therefore gives a count back at an orderly stop, under three conditions:

1. The stopping start completed (`Started`). A start that failed keeps its count.
2. This image wrote and flushed that count, and no confirmation cleared it since.
3. A start in this boot was confirmed. A durable confirmation writes the mark
   `Parameters\GuardBoot\Confirmed` = 1 in a volatile key, which the
   configuration manager drops at every reboot.

The count never goes below zero. The protection across reboots does not change:
a crash or a power loss never reaches the stop, dxgkrnl does not stop the
adapter at shutdown, and a new boot has no mark until its own start is
confirmed. A start that is not confirmed and is stopped in an unconfirmed boot
keeps its count. Host test: `driver/kmd/test/guard_start_test.c`.

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
