# M600: clean-source UMD desktop trial

DWM023, unit A, exactUMD5C74BF98 from clean commita0ad8af5, hostedICD3508416F,
unchanged KMD152. The bounded run completes180.094s with DWM9948. It adds no
per-draw flush/wait diagnostic. All retained module samples identify this UMD.

Image:8000 static composition pixels match CPU reference, primary readback and
GDI capture. All five dynamic BMP/PNG pairs, final BMP/GDI and CPU baseline pass
the cyan-shape diagnostic. These are sampled checks; banded primary capture
can mix scanout surfaces and cannot be a coherent whole-frame oracle (M593).
The requested3-second host sound was played after the GPU module witness.
No owner visual verdict has been received for this run; M595's observation
applies to the earlier49AFB21F binary and is not silently transferred here.

Execution:3634 DWM-owned matched DMA start/stop pairs, zero events/buffers lost,
no pending/unmatched/duplicate pairs, matching submission/completion IDs,
no preemptions.62 queue-fence samples are monotonic;53 have completed=submitted.
At sampled Present3240, submitted=completed3253. This complements ETW; the
fence IDs are not asserted to be one-to-one ETW IDs or scanout timestamps.
The clean frontend flushes then calls Bc250QueuePresentWait, which queues
pfnWaitForSynchronizationObjectFromGpuCb before pfnPresentCb; it signals the
present fence afterwards. The CPU fence_finish branch is for non-hosted use.

Mapping:167 snapshots fully reconcile totals and classified buckets with zero
overflow and no omitted calls/bytes. Full-frame buckets contain one WRITE and
six READ requests; six reads are consistent with six harness GDI captures,
without call-stack attribution. All persistent requests are seven24000-byte
descriptor maps and one1MiB uploader map, no persistent image maps. KMD blits
and translated-source counters stay716 at start/end. These are logical map
requests and completed blit counts, not measurements of each CPU store. The
M562/M596 source-path audit and clean-source buildM597 remain part of interpreting
these observations. No recurring full-frame map is observed in this workload;
this does not prove arbitrary workloads or remove remaining G0 lifecycle and
engine-blit requirements.

Both baseline hashes93B1D1FD/8279AC7F verified restored, CPU DWM4596, all023
tasks removed. Raw images/logs/ETW remain private, hashes retained. No permanent
promotion or full G0 acceptance claim.
