# M457 - Automatic full-WDDM confirmation, candidate 145

2026-09-25, unit A. Warm PnP update from144; Windows and DWM retained.

## Implementation and host controls

A typed96-byte ABI reads adapter-owned synchronized health storage without BAR
access or scheduler idling. Each device start has a boot-monotonic identity;
visibility/mode/power/fault changes invalidate its epoch. The presentation
witness counts distinct successful hardware programming followed by verified
completion. PrimarySequence alone was insufficient because it also advanced
on failed or unchanged requests. Repeated vblanks and old-buffer fallback
reports do not create progress.

The interactive monitor requires60seconds of fresh advancing witnesses for one
generation/epoch. Confirmation rechecks the live snapshot, then requires a
successful native registry flush. A lifecycle mutex serializes PASSIVE changes;
DPC faults immediately invalidate the epoch and cannot inherit confirmation.
The decision certifies the preceding interval, never future fault-free operation.
Read rundown protects admitted queries; OS callback lifetime is still required
for initial hAdapter entry. No spin lock is held across persistence.

Actual-source tests: kernelhealth554/0, flip129/0, visibility810/0, monitor120/0,
client17/0 and existingtiming205/0. Negative controls detect ignored kernel flush,
unchanged presentation count and a client read that requests hardware idling.
Complete signed KMD, CLI/controlDLL and monitor builds pass. Monitor also passes
six Python ABI/stage checks and34inventory checks. These mocked interleavings
are not a multicore stress test or hardware recovery proof.

## Identities

Frozen source: scratch/m9/combined145-build-source,318files, manifest here.
Only files changed/new versus frozen144 are additionally copied under source/.
KMD0.7.145.1 SYS: ED7B2E0735A41048DDA1428FB4A759C32193A231D5A26A5C2FEAD5D8407903CB.
Package: scratch/build/bc250kmd-07145/package-umd.
CLI: 68B346AFFF020631F91B1C26F2B51A656762346D53D6B168992F7A97CF9E3FD4.
ControlDLL: 02412B52C68E4B02030BBD26E33CD948FADDE74AADD14406813B2EB46B2B0962.
Monitor: 4F5597D3C34AF92A205CB3CC4AF031B1EB2983C6F477E89334EE04B5D2F7A875.
Native1000MHz/VID116; temperature66.375C in final sample.
DesktopUMD SHA D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D
was checked before PnP and retained. Rendering remains llvmpipe CPU+hardwareDCN.

## Actual automatic confirmation

The new monitor5780 starts01:42:13. PnP enable starts01:42:54. Driver log records
policy2 and checked guard increment0->1 at0.136s before hardware admission.
The install trace then records flags7, generation29824235770, epoch5, four
completed primaries, ready7336ms and UnconfirmedStarts1. No explicit confirm
runs after this start; the explicit confirmation in the installer precedes
PnP disable of144.

At01:44:08.290 the monitor logs successful automatic confirmation after60seconds
observed progress: the same generation/epoch,82completed primaries. At01:44:11
health returns flags15,87completed, ready73928ms and budget0. The later01:45:18
follow-up returns the same confirmed identity with222completions and budget0.
CONFIRMED is published only after checked kernel persistence. The individual
persistence log line has already wrapped out of the ring; do not claim it was
retained. The typed confirmation result, monitor log and source-pinned branch
are the persistence witnesses here.

The standalone acceptance script started late and exits with 'Did not observe
genuine pending start'. Preserve that failure. It observed the already confirmed
start, not a driver failure. The earlier installer provides the missing pending
sample for the exact same generation and OS boot; the follow-up verifies the
confirmed state. No restart, fake counter increment or rerun was used to obtain
another interval. Initial continuous samples are enforced by the tested monitor
policy, not independently recorded by an external sampler on every poll.

## Display and GPU controls

The owner explicitly confirms correct physical image and smooth cursor/overlay
on145. Windows boot00:53:18 and DWM1720/start00:54:20 are retained through01:47:24.
Finalhealth: generation29824235770,epoch5,flags15,376completions,age730ms.

The64MiB residency control passes three eviction/re-residency cycles and four
full GPU readbacks with all words matching. GFX256/256, paging1870/1870, zero
timeouts/refusals and no TDR. Probe identity is checked in the harness. Model
and shader acceptance remains the M455144 record; it was not repeated here.

## Limits and artifact handling

This establishes one warm-PnP automatic confirmation, not AC-cold start,
repeated production boots, power resume, hardware reset/preemption, a soak,
matched performance or full M9 closure. Other audit memory/DMA gaps remain.
Logs are transcoded toUTF8; device-instance/UUID lines are removed where present.
Raw originals remain in scratch. No firmware/NVRAM or OS-boot change was made.
