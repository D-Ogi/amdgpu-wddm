# Idle buffer DISCARD and subsequent NO_OVERWRITE writes

Hypothesis: returning failure from invalidate_buffer for an already idle BO
allows a full-buffer staging upload to race later direct NO_OVERWRITE writes.
Diagnostic099 reproduces the M579 pixel failure and shows its first DISCARD
using staging, followed by63 direct maps to the destination. Later DISCARDs
rename successfully. Exact diagnostics remain private pending analysis.

Treat the no-usage branch as successful invalidation: the old contents can
already be discarded without allocating or waiting. The caller then marks
the direct map unsynchronized. Sparse and fixed-address early exits stay as
before. This adds no CPU wait and changes no presentation copy path.

Apply idle-discard.patch after buffer-ownership.patch using the manifest's
exact source hashes. Build candidate044, UMD67E4C9F5, remove diagnostic prints.
Run unchanged EXE97110446 with hosted ICD3508416F as control100, keeping DWM
on CPU and restoring baseline ICD/UMD in finally. Require all48 state images
to match WARP094/CPU095 in both submission patterns. A reproduced failure
rejects this candidate; a pass requires sharing/Present regression and a
separate bounded DWM test before any claim about BD-043 or G0.
