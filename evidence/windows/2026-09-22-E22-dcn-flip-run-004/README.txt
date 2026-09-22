E22 run 004 (step 3, the interrupt question), 2026-09-22 09:58-10:03: bc250kmd 0.7.24 (85fbf5c) on unit A, owner
at the monitor. Written to test the inference of M98, that the IH ring is what the VUPDATE interrupt was missing.

run-004-script.sh: e19_target.ps1 -Phase gate -Full 1 -GpuVa 1 -Engines 1 -Blit 1 -VidPnFlip 1 (the paging node
stays closed and `gfx run` is never called - that is the step that hung the machine in E24 run 001), then the
bring-up's gart enable, psp load and ih init, 40 s of ordinary desktop, the log summary, `ih state`, a scanout
read-back, and the undo (psp unload, gart restore, gate closed).
run-004-console.txt is the unedited console. run-004-scanout-half.png is what HUBP0 was scanning out, half scale.
