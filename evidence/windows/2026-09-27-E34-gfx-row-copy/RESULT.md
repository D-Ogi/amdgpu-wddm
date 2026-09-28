# M606: bounded GFX row-copy builder

The geometry-to-packet builder passes decoded pixel-copy tests at30 command-buffer
capacities, preserving padding and untouched pixels. A wide two-row plan resumes
within and across rows in four packets. Each batch has only first RAW_WAIT and
final CP_SYNC. Invalid full footprints, cursor/pitch, overlap, wraparound and
insufficient capacity leave packet output untouched; empty plans complete without
output. Host/kernel-flag /W4 /WX compilation and all11 quick gates pass. The
standalone copy-emitter regression remains part of the suite. Parent e4c4887;
source hashes and raw local results attached.

This is internal command construction only. DDI multipass adaptation, allocation
mapping/residency/lifetime, cache ordering, actual GFX execution and WDDM fence
retirement remain outstanding. No hardware packet submission, deployment or
interop-capability change was made. G0 remains open; lab use is coordinated
separately with the ongoing game presentation controls.
