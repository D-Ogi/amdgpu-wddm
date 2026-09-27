# M604: GPU Blt geometry planner

The allocation-relative planner passes4290 dirty-rectangle combinations against
an independent pixel oracle, preserving padding and untouched pixels with
unequal pitches. Additional negative/large-offset cases pass. MSVC host build
and kernel-flag compilation pass with /W4 /WX. All nine quick quality gates pass,
including the newly enforced blit-plan gate. Source hashes and raw local results
are attached; build parent is d617676.

The module returns a validated row-copy plan, handles source translation and
clipping, rejects scaling/conversion/out-of-bounds input and clears failed/empty
outputs. It does not emit packets, resolve residency, account fences or replace
WddmPresentBlit yet. No lab operation or new driver artifact was used here.
KMD153 and CPU DWM remain as in M603. GPU engine integration and G0 remain open.
