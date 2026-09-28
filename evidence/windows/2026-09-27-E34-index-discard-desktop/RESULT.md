# M593: narrow index DISCARD fix on the desktop

DWM021 on unit A, UMD49AFB21F/ICD3508416F, runs185.650s with3792 loss-free
matched DMA pairs attributed to DWM7596. Submission/completion IDs agree,
no preemptions, unmatched/pending/duplicate pairs. No added per-draw flush/wait
or broad state re-emission. Static8000-pixel composition ROIs pass.

All five sampled moving-window BMP/PNG pairs and final GDI PNG pass the cyan
shape diagnostic. Final gpu.bmp fails (fill0.9472). Preserve this failure:
fbdump reads64-row bands with a fresh scanout address each call (dcn.c FbdumpEscape,
CLI Fbdump). Its log proves multiple surface bases in this BMP, with a base change
at row192 exactly where cyan extents change from x624..839 to x632..863. It is
a mixed-frame capture, not a coherent whole-frame shape oracle. This explains
the check's limitation; it does not prove every rendered frame correct. Future
exposed-area validation needs a settled/stepped capture control. No owner visual
verdict was received, and the017 residual shrink artifact remains unverified.

Map audit: no bucket overflow. Final image buckets contain one1920x1200 WRITE
request and six1920x1200 READ requests, consistent with startup plus the six GDI
captures in the harness. Attribution is not call-stack proven. Persistent maps
are seven24000-byte descriptor maps and one1MiB uploader map; no persistent image
bucket is reported. KMD blit and translated-source counters remain698 at start/end.
These are bounded map/counter observations, not measured CPU stores or a complete
new source-path audit proving absence of every possible frame-copy path.

Baseline ICD93B1D1FD/CPU UMD8279AC7F verified restored, CPU DWM12636; all021
tasks removed. No promotion. BD-043/G0 remain open pending stronger dynamic visual
acceptance and final artifact/source/no-copy validation. Raw ETW/images/logs
remain private; their hashes and derived results are retained.
