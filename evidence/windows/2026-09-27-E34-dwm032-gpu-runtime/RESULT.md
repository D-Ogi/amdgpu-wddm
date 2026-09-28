# DWM032: bounded GPU desktop with KMD164 (M672)

Unit A, 2026-09-27. Exact runner92cca5b, KMD164/9B9B99D3, clean UMD5C74BF98,
direct ICD3508416F. The shared-stream and process-observation fixes let startup
complete. DWM8548 starts12:54:29.9179669Z; CreateDevice bracket12:54:32.088001Z to
12:54:32.868590Z returns success. Readiness observation12:54:35.133845Z at5.434s is
separate from the186.0786884-second measured interval. No per-draw serialization.

Image:8000 static red/blended pixels match expected values in CPU baseline, final
primary and GDI screenshot. Eight dynamic BMP/PNG samples and both final captures
pass the cyan-shape check. CPU baseline shape fails fill0.9286 (33044pixels in bbox
632,169,888,308); preserve this failed control. Banded primary capture can mix surfaces
(M593); this record does not independently establish that as the cause. There is no
owner visual verdict or coherent whole-desktop pixel oracle for032.

Execution:3372 matched DWM-owned DMA start/stop pairs, matching IDs,zero trace loss,
no pending/unmatched/duplicate pairs.58 sampled queue-fence records are monotonic
and caught up; last Present3000 has submitted=completed3013. Native node0 counters
are147/147 at first measured summary and3014/3014 at final,zero timeouts/refusals.
BGP1 submits stay8 with zero rejected/failed; these are startup engine copies, not
steady-state DWM draws. Their counters alone do not prove BGP1 copied pixel contents.

CPU-copy instruments:155 map snapshots reconcile every image/buffer request against
classified buckets,zero overflow/unclassified/persistent omissions. Final retained
snapshot9408 has one full-frame WRITE request and four READs, no recurring full-frame
WRITE. Persistent maps remain seven24000-byte descriptors and one1MiB uploader,
not images. KMD CPU blits/skips/translated sources stay0 at first/final summaries.
Logical map requests do not count each CPU store, and the final snapshot is not a
forced teardown total. Five GPU GDI captures exist but four full-frame READ requests
appear in the last snapshot; do not silently equate them or claim exact readback
attribution. Existing source-path audits M562/M596 and clean-source binding M597 are
needed to interpret this bounded absence of recurring full-frame CPU copies.

Automatic rollback CPU9852 at12:57:53Z,done12:57:55Z. Collector11936/start12:54:26.1369799Z
ends12:57:27.5513323Z,159samples,no timeout. Archive pulled,all032 tasks removed.
Fresh13:00:03Z exact164/8279AC7F/CF3948D6,health15/guard0,gates0,1000MHz/66.875C,
same boot,no test processes. No promotion. G0 remains open: retain the image-control
limitation and complete BGP1 content and full CPU-copy-path validation. Successful032
also does not explain away030's intermittent device-stage enumeration failure.

Full images/logs/ETW stay private with hashes. Published summaries contain only the
controlled workload's data; no unrelated process/image list from Kernel-Process ETW.
