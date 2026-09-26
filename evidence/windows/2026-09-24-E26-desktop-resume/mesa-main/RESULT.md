# M412 - Current upstream Mesa desktop with LLVM23.1.2

Unit A, 2026-09-24. Mesa main f9a2d34a19c496e552b6cd603e7807f1025a2e98
(26.3.0-devel, upstream commit2026-09-24T10:30:37Z) now supplies the lab's
D3D desktop UMD, linked to LLVM23.1.2 commit85ac560262434c9ccfc0c183ec22d4138ed647fb.
A separate worktree preserves the previous Mesa/WDDM2 source and artifacts.
The Gallium patch retains existing presentation, resource identity rotation,
render-fence wait, shader translation and diagnostics. One three-way conflict
was resolved by retaining upstream's removal of TXF_LZ while adding SAMPLE_I
beside TXF. Exact consolidated patch and successful reverse check are retained.
MSVC19.44, Meson1.12.0, MT/debugoptimized, static LLVM, no API adaptation needed.

Eight actual TGSI/NIR controls pass.1236 upstream llvmpipe arithmetic JIT
checks pass. Live UMD C:\BC250\m13\mesa-main-umd\bc250d3d.dll SHA256:
D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D.
DWM4448 runs from13:58:18 through14:01:27; runtime renderer is
llvmpipe (LLVM23.1.2,256bits), matching the loaded-module/overlay manifest.
KMD0.7.127.1 and Windows boot11:44:14 are unchanged. Graphics reloaded only;
no OS/AC reset. The recorded Dwminit exit at13:58:18 is the requested restart.

Actual scanout shows the desktop and color-control window. Shared red/blue
reads have0/2048 mismatches both ways; green staging0/307200, Present/device-
removed status0, control native exit0. Concurrent64KiB residency passes3cycles
and4full GPU readbacks: GFX4/4, SDMA829/829, zero timeout/refusal, no TDR.
Final407 hardware flips,12533 VSync acknowledgements,3deferred,0refused,
guard0,T66.8C. Deferred flips are reported, not discarded as errors or hidden.
104 two-draw samples from frame14 have median draw+wait4.591ms, max6.043ms.
This is not a matched benchmark; newer Mesa/LLVM performance is not established.

Scope: desktop D3D module only. The separate working Vulkan RADV/WDDM2 ICD
remains based on the earlier fork. Official main lacks that winsys; updating
it requires a separate port and compute/model validation. DWM still renders
on the CPU; GPU D3D acceleration, full shader conformance, owner input feedback
on this build and30minute desktop acceptance remain open. M9 is not closed.
M411/M410 binaries remain available for rollback.

Text collection redacts interface and PCI instance identity and preserves
line endings. Raw scanouts remain outside the repo because the overlay shows
private network data; published crops omit it. The manifest captures exact
versions and hashes without exposing addresses.

PROVENANCE: Mesa MIT; LLVM Apache-2.0 WITH LLVM-exception.
