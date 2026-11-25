BD-403: DXGK_DRIVERCAPS.MiscCaps.SupportContextlessPresent. Static reading, development PC, 2026-11-25.
No lab run. The binaries are the copies in the workspace symbol store (scratch\symbols), from unit A's
Windows 11 22631. The HLK failure that started this work is on 26200 (tester.39 rc3). 26200 was not read.

Files
  dxgkrnl-static-reading.txt  the disassembly of the four places below, with the binary hashes and PDB ids.

What the HLK says
  Job "WDDM2.4 Mandatory Caps", featurecheck.dll, FeatureCheck::VerifyWddm24Caps, main.cpp line 65:
  "SUCCEEDED(pCaps->SupportContextlessPresent ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL): SupportContextlessPresent
  required on WDDM2.4+". The log reports "Driver version is 2900" and "MinimumDriverVersion=WDDM_2_4". Source:
  scratch\t39\slot-full-cks-run\export-05a86c55-839\logs\log-00036.bin (outside the repository). The test stops at
  this first error, so the log does not show the other items of the test.

What the pages and the header say
  WDK 10.0.26100 shared/d3dkmddi.h line 2467: "UINT SupportContextlessPresent : 1;", the first bit of
  DXGK_DRIVERCAPS.MiscCaps, in the DXGKDDI_INTERFACE_VERSION_WDDM2_4 block that starts at line 2462. No comment.
  ns-d3dkmddi-_dxgk_drivercaps.md line 263: "Supports null context in DDI calls. When this value is set, the OS
  will pass NULL context in present related DDIs. Supported starting in WDDM 2.4."
  ns-d3dkmdt-_d3dkmt_wddm_2_0_caps.md line 63: the user-mode copy, "Supports contextless present display."
  The pages do not name the DDIs and give no status rules.

What dxgkrnl 10.0.22621.6197 does (dxgkrnl-static-reading.txt)
  1. DXGADAPTER::Initialize+0xe8a (VA 0x1c01fdafe): "cmp dword ptr [rsi+0xb04], 0x960; jl". 0x960 is 2400, the
     driver version in the decimal form that the HLK log prints (2900). From 2.4 only: when the dword at +0xae8
     is 0x80 or more, the start fails with code 0x1cf4; else "and al, 0x1; mov byte ptr [rsi+0xb3a], al".
     Reading of the layout: +0xae8 is MiscCaps.Value. Bit 0 is SupportContextlessPresent, bit 4 (tested next,
     with a DDI pointer at +0x570) is IndependentVidPnVSyncControl, and 0x80 is the first bit that 22621 (WDDM
     3.1) does not know. This identification is from the bit layout of d3dkmddi.h, not from a type record.
  2. PrepareIndependentFlipToken (VA 0x1c01bea58), the guard at 0x1c01bec00: for a token without a context
     (rbx 0), the function continues only when the byte at +0xb3a is set.
  3. PrepareIndependentFlipToken, VA 0x1c01c0b27-0x1c01c0b45: "cmp byte ptr [rax+0xb3a], r14b; mov rdx, r14;
     jne; mov rdx, qword ptr [rbx+0xb8]; ...; call ADAPTER_RENDER::DdiPresent". With the byte set, hContext is 0,
     also when the token has a context. Without it, hContext is the field at +0xb8 of the context.
     The Ghidra pseudocode of the same function (rea, Ghidra 12.1.4) shows the DXGKARG_PRESENT that it builds:
     0xA8 bytes cleared, Flags 0x2000 (RedirectedFlip) with the token's bits 0x1700 (stereo and
     FlipWithMultiPlaneOverlay), pAllocationInfo with NumSrcAllocations entries of 0x20 bytes from index 0,
     PrivateDriverDataSize and pPrivateDriverData from the token. pDmaBuffer and DmaSize stay 0.
  4. SubmitPresentHistoryToken calls PrepareIndependentFlipToken for a context, or without a context when the
     byte is set. Its strings: "Sync Object and pPresentRedirected must be specified when context is null" and
     "Context can not be with sync object". The submission without a context goes to dxgmms2
     VidSchSubmitCommandContextless (VA 0x1c00409c0 of dxgmms2), which takes a sync object and a VidSch device.
  The other readers of the byte: DXGCONTEXT::SubmitPresentMultiPlaneOverlays3 (VA 0x1c01c497b, 0x1c01c4d5a) and
  DXGCONTEXT::SubmitPresentLda (linked adapters, 0x1c0340c19, 0x1c0340ddc). Not decoded. DXGCONTEXT::SubmitPresent
  and SubmitPresentWithDmaBuffer also call DdiPresent and do not read the byte, so a Blt keeps its context.

What is not known
  - The other WDDM 2.4 items of VerifyWddm24Caps: featurecheck.dll is not in the workspace.
  - Whether 26200 and 26300 behave the same. Only 22621.6197 was read.
  - What SubmitPresentMultiPlaneOverlays3 changes with the byte set.
