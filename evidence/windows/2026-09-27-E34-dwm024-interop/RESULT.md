# DWM024: interop setting exercised; no sustained CDD Blts captured

Unit A,2026-09-27. Runner5f5d248, exact KMD1608E676C81.
Interop changed0->1 through an in-session adapter restart, with GPU Present0.
The router enabled only after refreshing adapter LUID. DWM8364 loaded exact
hosted UMD5C74BF98 and direct ICD3508416F; the measured interval was182.2867594s.
No OS reboot. This was a shape-capture experiment using diagnostic E26P for
CDD Blts, not the BGP1 GPU producer.

ETW attributes3258 matched DMA start/stop pairs to DWM with zero lost events,
zero lost buffers, unmatched pairs or duplicate starts. This is bounded GPU
attribution, not a full correctness or copy-exclusion proof.

The first retained summaries already show14 E26P Blts and0 skips; the ending
summary still shows14/0. Their initial destination pair was not captured:
the after-enable log ends before these Presents and later ring logs have
overwritten the detail. Do not assert all14 occurred before GPU DWM startup.
There were no additional E26P Blts over the measured steady animation interval.
BGP1 counts remain0 because its gate was0. The requested sustained CDD-to-DWM
Blt shape is therefore unproven; retaining the desktop does not pass that gate.

8000 static composition pixels match expected colors in baseline primary,
GPU primary and screenshot. Eight dynamic BMP/PNG samples pass the bounded
moving-shape check. Final screen.png passes; final gpu.bmp fails (fill0.9073).
A concurrently changing primary capture is a possible explanation, not proven.
No owner visual verdict and no claim that all artifacts are absent.

Normal finally rollback restored baseline UMD8279AC7F and ICD93B1D1FD, latched
interop0 by adapter restart, then restored CPU DWM7656. The pending marker was
removed only after a latched-setting witness. Independent cleanup checked
both settings and removed all three tasks before the sleeping watchdog fired.
Final08:58:32Z: health15/guard0,1000MHz/VID116,66.875C, no test processes.
Exact159 package remains available. KMD160 stays diagnostic.

Full logs, images and ETL remain under scratch/g0-hosted/dwm024; hashes recorded.
JSON/ETW proof copied unchanged; selected KMD text preserves relevant original
lines with encoding conversion. No images published. G0 remains open.
Next: capture actual early Blt descriptors and confirm capability delivery to
dxgkrnl before interpreting the lack of sustained interop copies.
