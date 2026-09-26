# Current Mesa desktop trial - 2026-09-24

Hypothesis: upstream Mesa main f9a2d34a19c496e552b6cd603e7807f1025a2e98
(26.3.0-devel, 2026-09-24) retains the working llvmpipe/D3D desktop with
LLVM23.1.2 and our existing presentation, resource rotation and TGSI fixes.
This promotes only the D3D desktop module. Existing RADV/WDDM2 compute ICD
remains separate: its upstream branch is absent from Mesa main and requires
an independent port and full compute acceptance before replacement.

Use a separate source/build tree and preserve the M411 LLVM23/Mesa-July
artifact. The Gallium patch applies with one semantic conflict: upstream
removed TXF_LZ; preserve its removal and add SAMPLE_I beside TXF. All eight
TGSI controls and1236 JIT arithmetic checks pass before deployment.

Reload graphics on KMD127 without an OS/AC restart. Verify exact DWM module
hash/runtime renderer, visible scanout, two-device red/blue and green content,
concurrent64KiB GPU residency, and frame timings including render-fence wait.
Retain existing gates and clocks. Failure means record evidence and restore
the prior module, not claim migration acceptance from the build alone.

PROVENANCE: Mesa MIT; LLVM Apache-2.0 WITH LLVM-exception.
