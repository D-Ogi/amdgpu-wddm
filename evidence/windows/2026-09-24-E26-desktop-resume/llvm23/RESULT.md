# M411 - LLVM23.1.2 deployed and validated

Unit A, 2026-09-24. Mesa remains801c9763c6043f0de8408e905a5324eea06d81d7
with the M410 Gallium fixes. LLVM23.1.2 commit85ac560262434c9ccfc0c183ec22d4138ed647fb
builds Release MT X86-only. The first sparse build lacked libc/shared/math.h;
adding upstream libc to sparse checkout fixed it without source edits.
Mesa resolves LLVM23.1.2 and builds without LLVM API changes. Actual TGSI/NIR
controls pass8/8; upstream llvmpipe arithmetic JIT control passes1236 checks.

UMD C:\BC250\m13\llvm23-umd\bc250d3d.dll SHA256:
916D14B8CF3AA97E32C22D5AFC92F2BAEF82539C466B2654A5D70DA4CBC0F9DB.
KMD0.7.127.1 remains unchanged, Windows boot11:44:14 remains unchanged.
DWM2140 starts13:52:06 and survives through final13:55:35. Runtime reports
llvmpipe (LLVM23.1.2,256bits); loaded path/hash and overlay agree. Actual
scanout shows desktop/overlay and the color-control window. No OS/AC reset.

Two-device red/blue:0/2048 mismatches both ways; green staging0/307200.
Present/device-removed status0, control exit0. Concurrent64KiB residency
passes3cycles and4full GPU readbacks; GFX4/4, SDMA827/827, no timeout/refusal/TDR.
Final436 hardware flips,13707 VSync acknowledgements, guard0,T66.8C.
107 two-draw samples from frame14 have median draw+wait4.502ms, max5.676ms.
M410 median was3.9055ms from a different sample. This is not a matched benchmark
and establishes neither an LLVM23 speedup nor an attributable regression.

LLVM19 and prior UMDs remain available for rollback. CPU JIT drawing remains;
GPU D3D acceleration, broad shader conformance and30minute desktop acceptance
remain open. Owner fluid-cursor confirmation applies to M410, not this trial.
Text copies redact interface and PCI instance identity only, preserving line
endings; public scanout crops omit the overlay's private network data.

PROVENANCE: Mesa MIT; LLVM Apache-2.0 WITH LLVM-exception.
