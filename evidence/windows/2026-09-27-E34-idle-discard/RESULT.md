# M582: idle DISCARD avoids a staging/direct-map hazard

Negative diagnostic099 reproduces M579 with EXE97110446, UMD645D4DFD and
ICD3508416F:8 state images pass, then vb-nooverwrite fails4032/4096 pixels.
The768 recorded16KiB maps show map512 (the first DISCARD of that case)
using staging, followed by63 direct destination maps. Later DISCARDs rename
backing objects. The full pointer-bearing diagnostics stay private.

Source: invalidate_buffer clears valid_buffer_range, finds no remaining BO
usage, then returns false. Its caller permits a staging fallback; completion
checks can still observe a recycled batch-usage pointer. A queued full-range
copy can overwrite later direct NO_OVERWRITE writes. Return true for this
already idle allocation so the caller maps it directly and unsynchronized.
No new CPU wait or full-frame presentation copy is introduced.

Candidate044 UMD67E4C9F5 removes diagnostics and applies only that one-line
change over M579 UMD8CDF2C85. Exact patch replay matches source hashes.
Control100 passes48 state images /196608 pixels,3072 draws across both
Flush patterns, plus the8 preceding graphics cases. Expected hashes match
the M578 WARP094/CPU095 positive controls. Exit0, baselines restored and
CPU DWM4400 unchanged. No DWM switch, no candidate promotion.

This fixes the reproduced offscreen failure. Sharing/Present regressions
and a new bounded DWM test remain required; BD-043 and G0 are still open.
Unit A,2026-09-27. Parent4e4629e. Raw KMD/UMD logs retained privately.
