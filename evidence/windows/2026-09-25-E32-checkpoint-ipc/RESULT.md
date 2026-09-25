# M478: immutable checkpoint markers
Unit A, 2026-09-25. Harness correction; M11 remains open.

## Stopped soak-01
The worker stopped at 2026-09-25T09:22:46.514Z, elapsed 4146.700seconds,
after87 fully accepted cycles. Cycle88 completed its native workloads but failed
while waiting for checkpoint acknowledgment. ReadAllText(checkpoint.ack) collided
with the monitor's WriteAllText on that same file. The monitor recorded this
worker failure and terminated; both actual task states were terminal.
No inference of GPU failure is made from this file-sharing exception.

The independent final readback found the same Windows boot, zero matching
TDR/bugcheck/live-kernel events, unchanged dump inventory and all input hashes
unchanged. It had positive read controls for both Windows event logs.
The complete stopped run is preserved, not resumed or added to a later interval.

## Correction
Each cycle now creates its own empty request and ACK markers. The worker checks
ACK existence without opening its contents; the monitor processes exactly the
next request after completing its pool/event checkpoint. CreateNew prevents
silent replacement of a previously published marker. The old request/ACK text
files are no longer used.

A host test extracts the actual WaitCheckpoint function from worker.ps1. It
reproduces the old sharing violation with an exclusively held ACK, verifies
the new wait succeeds while the same handle stays open, checks that a previous
cycle's ACK cannot satisfy a later cycle, and repeats250 held-handle controls.
253 actual-function checks pass.

## Lab control-04
Two full mixed cycles pass on unchanged KMD147 / UMD8279AC7F / ICD9C40083C.
All eight compute reference hashes, both complete GPU-offloaded model outputs
and600cube frames per cycle pass. All measured pool tags return to the initial
counts/bytes in five final samples. Temperature66.625-67.250C; no new event or
dump change and every final input hash matches. The audit rejects this short
control only on its three24-hour duration/launch requirements, as intended.

## Artifacts
soak-01.tar.gz and control-04.tar.gz contain the collected logs, native outputs,
inputs, pool/thermal ledgers, runner sources and final readback. The matching
*-files.json lists SHA256 of every archived file. Only adapter LUID text in
stderr was redacted; pool-current.txt (the transient all-system PoolMon dump)
was omitted because project-only snapshots and pool.jsonl are preserved.
Archive headers carry no owner names. Private complete copies remain under
scratch/m11/soak01-terminal and scratch/m11/control04-terminal.

No Windows restart, device refresh, KMD change, UMD change or ICD change occurred
for this correction. A fresh24-hour interval is required. The deliberate stuck
queue has not run.
