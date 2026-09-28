# DWM026: GPU Present built, consumer rejected

Runner e57dbe1, staged manifest efbd883. Exact KMD161 source6c93d74,
SYS40F7916F; hosted UMD5C74BF98/direct ICD3508416F unchanged. Full hashes
are in the committed trial manifest and input-hashes.json. Private raw archive
and ETL remain in scratch/g0-hosted/dwm026; public receipts contain no account
credentials, machine identifiers or user desktop images.

The new readiness gate passed on attempt1: all8000 expected CPU-composed pixels
matched before any DLL replacement or adapter restart. Target PS5/hash/rollback
preflight passed. GPU Present1/interop1 latched. Producer calls18/records18,
rotate10/refused0; consumer submits0/rejected5/failed0. CPU Blt0/skips0 in the
GPU-gated generation. This is a failed GPU Present test, not a no-copy success.

The retained startup logs have five node0 Present submissions (fences1..5,
flags2,4096-byte IBs) after SetRootPageTable calls. The aggregate rejection
branch does not identify which predicate failed; do not infer bad residency,
IRQL, root or record mismatch. Next action is bounded predicate diagnostics,
not weakening validation. Successful hosted DWM hardware work is separate:
917 matched DMA start/stop pairs for DWM9704, matching submission/completion
IDs, no preemption, no unmatched/pending/duplicate starts, zero ETW loss.
The copied analyzer deliberately requires an aborted interval and does not
label this as a180-second acceptance run.

The trial was deliberately rolled back on the observed rejection. Last measured
sample53.3469102s, done.success=false; the runner noticed the externally requested
DWM replacement and exited. Restore receipt10:10:35Z changes9704 toCPU11676.
Both gates restored0. Original collector1828/start10:09:30.9385851Z terminated
10:12:32.4330164Z,160 samples, no reader timeout. Identity/terminal guards passed
before cleanup/archive. All three tasks removed. Final independent snapshot
10:13:25.9121809Z: health15/guard0,1000MHz/VID116,66.875C,no test processes,
exact161 and baseline UMD8279AC7F/registered ICD93B1D1FD. No OS reboot.

No owner visual verdict and no complete image acceptance claimed. Two dynamic
captures remain private for further analysis. G0 remains open; zero successful
BGP1 submissions is sufficient to reject this trial irrespective of those images.
