# llvmpipe comparison plan - 2026-09-24

Hypothesis: replacing softpipe shader interpretation with LLVM JIT reduces DWM
render latency with the same visible overlay. User observed normal cursor
motion while the overlay was briefly stopped and renewed stalls on its return.
This identifies an update trigger, not the exact internal stall mechanism.

Build Mesa 801c9763c6043f0de8408e905a5324eea06d81d7 with the existing E26
branch-label, identity rotation and frame profile patches, plus
mesa-llvmpipe-present.patch. LLVM 19.1.7 matches this tree's Windows CI pin;
release MT X86-only LLVM, separate debugoptimized Mesa build directory.
Wait for the Gallium render fence before publishing a presentation allocation.
Record actual screen name and render wait separately from Draw and PresentCb.
Disable silent softpipe fallback in this diagnostic build.

Use existing KMD 0.7.127.1, display/compute gates and clocks. Preserve old UMD
paths for rollback. Change registered UMD, reload PnP once if required for cached
registration, and restart DWM. No routine Windows/AC restart. Collect real
scanout, DWM module/renderer witness, profile and TDR/queue state. Run existing
shared red/blue and green D3D readback controls and GPU residency content check.
If it fails, retain evidence and restore the profile UMD before further work.

Expected: same correct image and readbacks, substantially smaller draw plus
render-wait time. If correctness or latency fails, the hypothesis is not accepted.
This is CPU rasterization, not hardware Direct3D acceleration or M13 completion.

PROVENANCE: Mesa (MIT), llvm/llvm-project 19.1.7 (Apache-2.0 WITH LLVM-exception).
