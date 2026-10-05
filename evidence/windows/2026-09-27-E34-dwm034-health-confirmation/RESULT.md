# M676: DWM034 colour preflight precedes the healthy-start confirmation

Unit A, runner1366ec4. Exact KMD164/933f383, router4F55DA9D,
UMD5C74BF98/hostedICD3508416F; registered CPU baseline CF3948D6 preserved.
Hosted DWM5200 started14:01:20.0164818Z and passed the startup witness.
The recorded worker caught the preflight exception at14:01:29.0824004Z:
KMD164 health gate, Snapshot line21. No colour probe process or KMT present ran.
The main runner aborted at its first measurement iteration (reported measured
seconds0, not zero lifetime of the hosted DWM) and restored CPU DWM664.

The exact retained health line is:

    health abi=1 version=0x000700A4 flags=7 generation=109380886765 epoch=5 completed=65 age_ms=59 ready_ms=3947

The flags are a bitmask, not alternative integer states. The exact164 source
(driver/kmd/bc250kmd_escape.h at933f383) defines FULL1, READY2, VISIBLE4,
CONFIRMED8 and BC250_START_HEALTH_MIN_MS60000. start_health.c only adds the
confirmation bit for a durably confirmed matching generation/epoch. The monitor
StartConfirmationPolicy additionally requires60 seconds of observed progress.
At ready age3947ms a confirmation is not yet admissible. The worker's demand for
all four bits was valid, but launching it immediately after hosted startup was
premature. This directly explains034. It is consistent with033, whose exact
exception remains unrecorded; do not retroactively claim direct proof for033.

## Closure

The drain/disable receipts preserve original-process-before-adapter ordering.
Collector1332 ended14:04:08.7489550Z,159 samples,no reader timeout. All four tasks
were removed after collection. Archive5,380,428bytes and ETW42,991,616bytes retained.
Fresh14:07:05Z closure: same boot,CPU DWM664,exact164,UMD8279AC7F/ICDCF3948D6,
health15,1000MHz/VID116,66.625C,guard0 and both registry gates0. Cleanup verified
latched gates0. No active trial, no permanent promotion, no KMT route result.

## Prepared correction

DWM035 waits for the original health generation/epoch to report confirmed health
within90 seconds of its existing180-second measurement loop. It keeps the full
worker health requirement. Its50-second worker-receipt deadline starts at actual
launch; the desktop measurement is not extended. Missing/stale progress, changed
generation/epoch or unknown flags reject the trial.

PowerShell now uses a named Flags enum for the contract and checks required bits.
A test compares each enum member with bc250kmd_escape.h, replays the measured
flags7 line (wait), accepts confirmed fresh flags at65000ms, and rejects deadline,
generation,epoch,freshness and insufficient-ready-age controls. /W4 /WX build and
PS5 parsing pass. This is host validation of the correction, not a DWM035 runtime
result or completion of G0. Private full collection: scratch/g0-hosted/dwm034.
