# M568: GPU DWM completes the bounded run but fails dynamic visual correctness

**Visual result: failed.** During the moving/resizing window, the owner observed
trails at previous positions, broad flicker and corrupted colors/fragments.
The owner then observed the identical control on CPU DWM and reported none of
these problems. The cause is not yet established. This candidate is not promoted.

The owner shortened the proposed longer trial to three minutes. DWM012 records
185.75 seconds in the runner with GPU DWM PID4120 unchanged across31 samples.
UMD23F5269C and hosted ICD3508416F are verified in each sample. Temperature
ranges66.8-69.5 C. The runner's `success=true` means its bounded execution and
captures completed; it does not include the owner's independent visual verdict.
CPU comparison013 runs the same GDI executable for30 seconds on DWM8008.

## What the measurements do establish

- Red and half-alpha blue static interiors match expected values in CPU primary,
  GPU primary and screenshot:8000 pixels, no mismatch. Those fixed regions
  exclude the moving window and cannot establish dynamic visual correctness.
- DxgKrnl decoded with xperf reports zero lost events and buffers. Lifetime-aware
  context tracking attributes3818 matched DMA start/stop pairs to DWM4120, with
  no pending start, unmatched stop or duplicate start. Submission/completion IDs
  agree and no matched pair is preempted.
- The last sampled UMD audit reports3420 Presents and submitted/completed3423.
  Global KMD blits stay620 across179.54 seconds; node0 work deltas are3358/3358,
  with no added timeout/refusal. These global interval counts and ETW lifetime
  counts cover different boundaries and are not equated.
- CPU process time increases19.1875 seconds over179.57 seconds between samples,
  with diagnostics enabled. This is not a performance comparison or60 Hz proof.
- The map log contains16864 parseable bucket rows and one incomplete/interleaved
  row. No overflow is reported. Do not treat this partial audit as a new complete
  proof excluding every CPU full-frame copy.

The first analysis attempted to feed tracerpt's different CSV format into the
old xperf parser. Missing loss metadata caused rejection, not measured event
loss. The retained xperf output provides the zero-loss result above. Raw ETL,
both decoder outputs, full logs and screenshots remain private; hashes bind
them. Public JSON is extracted evidence, not a replacement raw trace.

## Restoration and preceding attempt

UMD8279AC7F and the newly promoted registered ICD93B1D1FD are restored and
hash-checked. Planned rollback replaces GPU DWM4120 with CPU DWM8008. All test
tasks are stopped and removed; CPU comparison retains PID8008. No KMD update
or OS restart occurred. Full artifact hashes are in manifests and runner source.

DWM011 is retained as an earlier harness failure: live Get-Content on DWM's
open log failed with a sharing error. Finally restored baseline and CPU DWM3164.
DWM012 monitors file metadata online and parses its contents after writer exit.
It does not reinterpret011 as a successful rendering test.

The owner subsequently requested a separate GPU window for phone photography,
DWM013. Its outcome is not included in this result. The next investigation must
isolate dynamic rendering/damage, sharing and presentation ordering, preserving
the CPU control. Static pixels and healthy fences do not close this defect or G0.
