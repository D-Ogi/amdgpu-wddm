# LLVM23.1.2 migration - 2026-09-24

Owner explicitly requests upgrading the working llvmpipe baseline to23.1.2.
Build upstream tag llvmorg-23.1.2, commit85ac560262434c9ccfc0c183ec22d4138ed647fb,
in a separate release MT X86-only build/install directory. Preserve LLVM19.1.7
and the working M410 UMD for rollback. Rebuild the same Mesa tree/patches against
LLVM23, adapting APIs only where source and compiler diagnostics require it.

Before deployment: verify llvm-config version, Mesa's resolved dependency,
linked module hash and actual TGSI-to-NIR controls. On unit A: preserve KMD127,
clocks and presentation gates, register the new UMD and reload graphics only
as needed. Verify DWM runtime renderer/version, actual scanout, D3D shared/pixel
controls, concurrent GPU residency and profile draw plus render-wait samples.
Update the overlay manifest and current baseline documentation with measured
results. A successful build alone is not migration acceptance. CPU rendering
remains CPU rendering and does not close M9 or hardware DWM acceleration.

PROVENANCE: LLVM Apache-2.0 WITH LLVM-exception; Mesa MIT.
