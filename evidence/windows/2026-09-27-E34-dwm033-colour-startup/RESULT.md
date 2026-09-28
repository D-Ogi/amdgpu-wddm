# M675: DWM033 hosted startup succeeds; colour worker fails before probe evidence

Unit A, 2026-09-27, runner e862252. Exact KMD164, router169B930A,
UMD5C74BF98 and hosted ICD3508416F. The staged manifest, hashes and PS5 parsing
passed; rollback163 artifact was verified. No permanent promotion.

Hosted DWM3148 (start13:46:51.9055451Z) passed readiness13:46:57.1487083Z,
sample2 after5705ms with successful CreateDevice and expected module hashes.
The runner retained47.0771766 measured seconds before aborting for the missing
colour worker terminal receipt. Final success=false. Retained KMD summary shows
interop1/GPU Present gate1 and8 submits,zero rejected/failed; these counters do
not attribute any work to the colour probe.

The interactive colour task terminated with LastTaskResult1. Its colour directory
was empty: no before.json, process identity, stdout, present log or terminal
receipt. Diagnostic readback verified the intended action and exact EXE228EBEC0,
private ICD0CD4A98D and workerD272E41C hashes. The worker writes before.json only
after preflight; its try/catch starts later. The specific exception was therefore
not retained. This is a failed test-harness startup, not evidence for either
acceptance or rejection of KMT Present. Do not infer that a probe process existed
from the registered task alone. The raw ETW is preserved for further diagnosis.

## Rollback and closure

The revised rollback order is directly witnessed: original DWM3148 drained
13:47:59.0838923Z; adapter disable began13:47:59.5222695Z,438ms later.
CPU DWM6708 was recorded13:48:05.1700716Z. New CPU DWM can encounter the adapter
transition; this is separate from the terminated hosted process.

Collector12560 remained live until13:49:48.5381546Z,158 samples,no reader timeout.
Collection waited for that original identity to terminate. Archive9,493,960bytes
and ETW66,060,288bytes were retrieved; all four tasks removed. Fresh13:51:43Z
closure verifies same boot,exact164,UMD8279AC7F/ICDCF3948D6,CPU DWM6708,health15,
1000MHz/VID116,66.875C,guard0 and both registry gates0. Cleanup also verified
latched interop0/GPU Present gate0. No test remains active.

## Next correction

A separate recorded-worker wrapper now writes a startup receipt before any worker
preflight and captures terminating exceptions, nonzero exits and output into
immutable per-attempt files. Host tests verify injected early failure, exit7,
success and duplicate rejection. The first test run stopped on PowerShell's
expected native stderr handling in the duplicate negative control; the revised
test handles that expected error and all four controls pass in a fresh directory.
It is not retroactively part of DWM033. The next trial must use it and retain the
actual preflight error rather than weakening the preconditions.

No KMT colour Present/content result, no new full-desktop acceptance and no G0
completion follow from this run. The preceding M674 CPU content control remains
valid. Private collection: scratch/g0-hosted/dwm033.
