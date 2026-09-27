# M595: owner-observed desktop after index DISCARD fix

DWM022 on unit A ran for 185.856 seconds with UMD49AFB21F and hosted
ICD3508416F. Exact artifact hashes are in manifest.json. This is the M591
narrow index-buffer cache invalidation fix, without the diagnostic per-draw
flush/wait or broad graphics-state re-emission.

The owner observed the animation and reported that the image looked good,
without glitches or artifacts. Observation duration was not measured. The
original report is retained locally; owner-observation.json contains the
English translation. This is bounded visual acceptance of this trial,
including the moving/shrinking animation, not a claim covering every workload.
The owner explicitly did not observe DWM018, DWM019 or DWM021; those runs
remain without an owner visual verdict.

ETW attributes 3971 matched DMA start/stop pairs to DWM7680, with zero lost
events/buffers, pending pairs, unmatched stops or duplicate starts. Submission
and completion IDs match; no preemptions. All 8000 exact static composition
pixels match the CPU positive control and GDI capture. All five dynamic
BMP/PNG pairs, final BMP/GDI captures and the CPU baseline pass the cyan-shape
diagnostic. These samples alone cannot exclude intermittent errors; primary
fbdump captures can mix surfaces across 64-row bands (M593).

The map-audit analyzer rejected this run because its 128-entry diagnostic
bucket table overflowed (maximum reported overflow 8). Do not infer complete
map coverage or absence of CPU frame copies from this run. KMD blit counters
remain 706 at start/end. G0 remains open for exact-artifact/source validation
and a complete no-frame-copy measurement.

Baseline ICD93B1D1FD and CPU UMD8279AC7F were hash-verified restored, CPU
DWM8116, and all three trial tasks removed. No permanent GPU promotion.
Raw images, logs and ETW remain private; hashes and derived results are retained.
