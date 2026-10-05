# Hosted allocation lifetime regression

M546, unit A, 2026-09-26. Candidate016 ICD3508416F / UMDC95C6D32.
Borrowed RADV BOs now reject CPU mapping. Hosted resource destruction waits for
Present and render completion, resets completed Zink batches, checks that only
the owning resource and object references remain, and releases them before
FreeGpuVirtualAddress/Deallocate2. A failed wait or unexpected reference rejects
this release rather than freeing an allocation still retained by Zink.

run031, unchanged two-buffer flip control:120 Presents, zero mismatches among
76800 final pixels, successful teardown and exit0. Both imported buffer releases
report resource refs1/object refs1 immediately before successful Deallocate.
Three GDI screen ROIs contain46800 exact red/blue/green pixels respectively.
Full-screen images stay outside Git because of unrelated desktop content; hashes
and ROI measurements are retained. Baseline UMD and ICD restored and hash checked;
CPU DWM1052 unchanged. No KMD change or restart.

Both DLL builds and all8 scoped gates pass. The two lifetime patches replay over
recorded UTF-8/LF inputs and match all5 output file hashes. This is not a clean
build of the complete accumulated Mesa stack. Apply after the loss patches.

This corrects the lifetime assumption in M541: dropping the Gallium reference
alone did not destroy the object retained by Zink batches. The new path explicitly
resets completed batches and checks references. It is a bounded normal-teardown
regression, not proof of every view/sharing combination or loss-time leak freedom.
Unexpected-reference paths still fail conservatively; BD-036 failure cleanup is
separate. GPU DWM and instrumented exclusion of CPU frame copies remain open.
PROVENANCE: Mesa MIT.
