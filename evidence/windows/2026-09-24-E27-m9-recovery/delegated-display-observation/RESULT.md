# M443 - Delegated hardware scanout observation

Main-session review accepts the bounded source fixes in [REPORT.md](REPORT.md).
BD-007 and BD-009 are source FIXED; BD-008 remains IN-PROGRESS because ambiguous
samples are intentionally deferred. No candidate containing these changes has
been installed. The deployed136 SYS1C93F357... predates them; the development
build EE83AE2C... carries the same version metadata but must never be confused
with it. Final source snapshot is source/, delta changes.patch.

Final121 publication checks,6726 DCN observation checks,35374 existing flip
checks and6892 geometry checks pass. Five targeted mutations are detected;
REPORT identifies final logs and earlier harness corrections. Final WDK build
is build-v2.log. Physical raster/latch/pacing acceptance remains open.

The new old-buffer report counter allows a future lab trial to distinguish
recovered pending-buffer vblanks from still-deferred samples. Generation
protection, the public75-register escape ABI and the write allow-list remain.
No claim of every-vblank coverage, VRR, interlace or own modesetting.
