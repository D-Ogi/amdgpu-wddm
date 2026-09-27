# Dynamic graphics-state isolation for BD-043

Hypothesis: an offscreen sequence of changing viewport, input layout/stride,
vertex streams, vertex-transform constants or disjoint buffer subrange writes
can reproduce DWM014's corrupt geometry independently of sharing/Present.

Run the `states` mode on WARP first, CPU llvmpipe second, hosted GPU third.
Six cases each issue four passes of64 permuted8x8 tiles into a64x64 target:
viewport plus default constant updates; alternating packed/split vertex streams
with different strides/offsets; DISCARD followed by NO_OVERWRITE vertex writes;
DISCARD vertex-transform constants;32-bit indexed triangle strips with nonzero
index/base-vertex offsets and NO_OVERWRITE; and an instance stream with nonzero
StartInstanceLocation. All NO_OVERWRITE ranges within a pass are disjoint.
Each new pass begins with DISCARD, allowing previous GPU work to remain pending.

Copy all four intermediate images into distinct staging textures before the
single end-of-case query wait. Compare98304 exact pixels against the tile-color
oracle, retaining each image hash. Repeat the six cases with Flush after each draw,
without CPU waits between draws:3072 draws and196608 pixels per renderer across
both submission patterns. The prior eight graphics checks also run.
Any mismatch, crash, timeout or restoration failure rejects the control.

Keep CPU DWM and baseline libraries restored between cases/runners. A pass
narrows these six sequences only; it cannot close the captured DWM014 failure.
