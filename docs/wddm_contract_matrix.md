# The WDDM start-up contract of the display miniport

What our miniport declares to dxgkrnl, what each declaration obliges it to supply, and where that obligation
is written down. It exists because the four refusals of E16 runs 001-004 were all of the same shape: a cap
or a version we set, and an element it silently required, found only after the adapter refused to start and
another trip to the lab. This file is the inventory; `tools/wddm_contract_check/` is the same table as code,
so that the next one is found by a build and not by a reboot.

Every rule in section (e) names a source. Three kinds, and nothing else:

| Tag | Means |
|---|---|
| `DXGKRNL` | a reading of unit A's own `dxgkrnl.sys` 10.0.22621.6199, offline, `cdb -z` with public symbols: `evidence/windows/2026-09-21-E16-run-002/dxgkrnl-static-reading.txt` and the longer scratch note it summarises |
| `LEARN` | a Microsoft documentation page, quoted with its section in the research note under `scratch\m7\` |
| `SAMPLE` | Microsoft's sample drivers, file:line (microsoft/graphics-driver-samples, MIT; instruments only, none of their code is in ours) |

Nothing here is from memory. A rule whose source would be "everyone knows" is a rule that does not belong.

State of the tree this describes: `driver/kmd/wddm.c`, 2198 lines, SHA-256 `4B6C6FC5...C98CFD3C` - commit
`78a44a0` (bc250kmd 0.7.14, 2026-09-22, stage C of the ring path) plus the working-tree changes to
`Bc250WddmPresent` that were in flight when this was regenerated. The file is under active development and
moved three times while this section was being written, so the line numbers below are a snapshot and drift
with every change. Regenerate them with
`python tools\wddm_contract_check\check.py --inventory` rather than trusting them after any change to
`WddmDriverCaps`, `WddmQuerySegment4`, `Bc250WddmQueryAdapterInfo`, `Bc250WddmCreateContext` or
`WddmBuildTable`. The member names, the values and the rules do not drift; only the line numbers do.

## (a) DXGK_QUERYADAPTERINFOTYPE: handled, refused, sizes

Answered. The size column is the exact buffer the compiled driver accepts; `<` is the guard it applies
before it writes anything. Every one of these was called on the host with the buffer shapes in section (f).

| # | Type | Handler | Size guard | Buffer | Answer |
|---|---|---|---|---|---|
| 1 | `DRIVERCAPS` | `WddmDriverCaps()` `wddm.c:1168` | `OutputDataSize < sizeof(*caps)`, `caps == NULL` | 576 | fills section (b), zeroes `OutputDataSize` first |
| 6 | `NUMPOWERCOMPONENTS` | inline `wddm.c:1180` | `OutputDataSize < sizeof(UINT)` **only** | 4 | `0` components |
| 10 | `HISTORYBUFFERPRECISION` | inline `wddm.c:1187` | `OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION)` **only** | 4 | `PrecisionBits = 0` |
| 11 | `QUERYSEGMENT4` | `WddmQuerySegment4()` `wddm.c:1171` | `OutputDataSize < sizeof(*out)`, `out == NULL`; pass 2 also `NbSegment < count`, `pSegmentDescriptor == NULL`, `SegmentDescriptorStride < sizeof(descriptor)` | 40 (+96/descriptor) | section (c) |
| 13 | `GPUMMUCAPS` | `WddmGpuMmuCaps()` `wddm.c:1174` | `OutputDataSize < sizeof(*caps)`, `caps == NULL` | 24 | section (d) |
| 14 | `PAGETABLELEVELDESC` | `WddmPageTableLevelDesc()` `wddm.c:1177` | `OutputDataSize < sizeof(*desc)`, `desc == NULL` | 20 | per level; `STATUS_INVALID_PARAMETER` above level 3 |

The two inline arms check the size and not the pointer. That is defect D1; see section (g).

Refused, all through one `default:` arm at `wddm.c:1195` with `STATUS_NOT_SUPPORTED`:

| # | Type | Why it must be refused |
|---|---|---|
| 0 | `UMDRIVERPRIVATE` | no UMD of ours yet; the stub package carries a name, not a driver |
| 2, 4, 5 | `QUERYSEGMENT`, `QUERYSEGMENT2`, `QUERYSEGMENT3` | a WDDM 2 driver must not answer the pre-WDDM2 segment queries (R15) |
| 7 | `POWERCOMPONENTINFO` | never asked once type 6 answers zero components |
| 15 | `PHYSICALADAPTERCAPS` | asked on the lab and tolerated (R16, E16 run 003) |
| 16 | `DISPLAY_DRIVERCAPS_EXTENSION` | asked on the lab and tolerated (M65) |
| 47 | `64BITONLYCAPS` | asked on the lab and tolerated; the WDK has no structure for it at all |

Types 15 and 47 are the ones the task singles out, and they are not refused on the strength of the
`default:` arm reading correctly. Both are called by `host/qai_test.c` with every buffer shape, including a
NULL pointer and a zero-size buffer, and asserted to return `STATUS_NOT_SUPPORTED` and touch nothing.

The lab-observed set is 1, 10, 11, 13, 14, 15, 16, 47 (M65). Type 6 is in our switch but has not been seen
asked for; it is answered because Learn lists it among the four a WDDM 2.x miniport must answer (R14).

## (b) DXGK_DRIVERCAPS: every member we set

Extracted from `WddmDriverCaps()` and confirmed against the compiled structure by the host harness, which
calls the real handler and prints what lands in the buffer. Members not listed are left at the zero the
handler writes over the whole `OutputDataSize` first.

| Member | Source text | Value | Line |
|---|---|---|---|
| `WDDMVersion` | `DXGKDDI_WDDMv2` | `0x2000` | 1034 |
| `HighestAcceptableAddress.QuadPart` | `-1` | no limit | 1035 |
| `SupportNonVGA` | `TRUE` | 1 | 1036 |
| `NumberOfSwizzlingRanges` | `0` | 0 | 1037 |
| `SupportSmoothRotation` | `TRUE` | 1 | 1047 |
| `SupportPerEngineTDR` | `TRUE` | 1 | 1048 |
| `SupportDirectFlip` | `TRUE` | 1 | 1049 |
| `SupportSurpriseRemoval` | `FALSE` | 0 | 1050 |
| `InterruptMessageNumber` | `0` | 0 (one MSI message, M38) | 1051 |
| `PresentationCaps.NoScreenToScreenBlt` | `1` | 1 | 1056 |
| `PresentationCaps.NoOverlapScreenBlt` | `1` | 1 | 1057 |
| `PresentationCaps.NoSameBitmapBitBlt` | `1` | 1 | 1058 |
| `PresentationCaps.NoSameBitmapOverlappedBitBlt` | `1` | 1 | 1059 |
| `PresentationCaps.NoSameBitmapAlphaBlend` | `1` | 1 | 1060 |
| `PresentationCaps.NoSameBitmapOverlappedAlphaBlend` | `1` | 1 | 1061 |
| `PresentationCaps.NoSameBitmapStretchBlt` | `1` | 1 | 1062 |
| `PresentationCaps.NoSameBitmapOverlappedStretchBlt` | `1` | 1 | 1063 |
| `PresentationCaps.NoSameBitmapTransparentBlt` | `1` | 1 | 1064 |
| `PresentationCaps.AlignmentShift` | `2` | 2 (4-byte pitch alignment) | 1069 |
| `SchedulingCaps.MultiEngineAware` | `1` | 1 | 1075 |
| `SchedulingCaps.PreemptionAware` | `1` | 1 | 1076 |
| `SchedulingCaps.LowIrqlPreemptCommand` | `1` | 1 | 1077 |
| `PreemptionCaps.GraphicsPreemptionGranularity` | `D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY` | 100 | 1078 |
| `PreemptionCaps.ComputePreemptionGranularity` | `D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY` | 100 | 1079 |
| `MemoryManagementCaps.VirtualAddressingSupported` | `1` | 1 | 1083 |
| `MemoryManagementCaps.GpuMmuSupported` | `1` | 1 | 1084 |
| `MemoryManagementCaps.PagingNode` | `BC250_WDDM_NODE_3D` | 0 | 1085 |
| `GpuEngineTopology.NbAsymetricProcessingNodes` | `BC250_WDDM_NODE_COUNT` | 1 | 1089 |
| `InternalGpuVirtualAddressRangeStart` | `0` | 0 | 1092 |
| `InternalGpuVirtualAddressRangeEnd` | `0` | 0 (no reserved internal range) | 1093 |
| `FlipCaps.FlipOnVSyncMmIo` | `1` | 1 | 1099 |
| `FlipCaps.FlipIndependent` | `1` | 1 | 1100 |
| `MaxQueuedFlipOnVSync` | `1` | 1 | 1101 |

As the compiled structure, from the host harness: `SchedulingCaps.Value = 0x00000045`,
`MemoryManagementCaps.Value = 0x00000060`, `FlipCaps.Value = 0x00000012`,
`PresentationCaps.Value = 0x030008FB`, `sizeof(DXGK_DRIVERCAPS) = 576`. The 576 matches what the lab's
dxgkrnl sized the buffer to (M63), which is the check that our interface version and its
`GetDriverCapsSizeFromDdiVersion` agree.

`PresentationCaps.NoCacheCoherentApertureMemory` was set until stage B and is not set now, which is why the
word is `0x030008FB` and no longer `0x230008FB`. Stage B declares an aperture segment with
`Flags.CacheCoherent = 1` (section (c)); keeping the cap would have told dxgkrnl the opposite of what the
segment says.

## (c) The segments

Two segments since stage B: the local one over the VRAM carve-out, and an aperture segment behind it. Both
are declared in two passes as `QUERYSEGMENT4` requires: pass 1 with `pSegmentDescriptor == NULL` returns the
count, pass 2 fills the descriptors. Segment ids are one-based, so the local segment is 1
(`BC250_WDDM_SEGMENT_VRAM`) and the aperture is 2 (`BC250_WDDM_SEGMENT_APERTURE`). They come as a pair: when
the carve-out is not identified, the count is 0 and neither is declared.

`DXGK_QUERYSEGMENTOUT4`:

| Member | Source text | Value | Line |
|---|---|---|---|
| `NbSegment` | `count` | 2, or 0 when the carve-out is not identified | 998 |
| `PagingBufferSegmentId` | `0` | 0 = system memory (changed from the local segment on 2026-09-21) | 1003 |
| `PagingBufferSize` | `BC250_WDDM_PAGING_BUFFER_BYTES` | 65536 | 1004 |
| `PagingBufferPrivateDataSize` | `0` | 0 | 1005 |

`DXGK_SEGMENTDESCRIPTOR4`, `sizeof` 96 bytes. The array is walked with the `SegmentDescriptorStride`
dxgkrnl passes in, not with `sizeof`: the stride is dxgkrnl's to choose and it may be larger than the
structure we compiled against. A stride smaller than the structure is refused with
`STATUS_INVALID_PARAMETER` before anything is written.

Segment 1, the local segment over the carve-out:

| Member | Source text | Observed | Line |
|---|---|---|---|
| `Flags.CpuVisible` | `1` | 1 | 973 |
| `Flags.LocalBudgetGroup` | `1` | 1 | 974 |
| `Flags.DirectFlip` | `1` | 1 | 975 |
| `BaseAddress.QuadPart` | `(LONGLONG)(Device->VramMcBase + offset)` | `0xF4008CA000` | 978 |
| `CpuTranslatedAddress.QuadPart` | `Device->VramPhysical.QuadPart + (LONGLONG)offset` | `0x2708CA000` | 979 |
| `Size` | `(SIZE_T)length` | `0x1FD736000` | 980 |

Flags word as compiled: `0x00080404`. Not set, deliberately: `Agp` (R22), `Aperture`,
`SupportsCpuHostAperture` (R29 would then demand the aperture DDIs), `PopulatedFromSystemMemory`.

Segment 2, the aperture:

| Member | Source text | Observed | Line |
|---|---|---|---|
| `Flags.Aperture` | `1` | 1 | 989 |
| `Flags.CacheCoherent` | `1` | 1 | 990 |
| `Flags.CpuVisible` | `1` | 1 | 991 |
| `BaseAddress.QuadPart` | `0` | 0 | 992 |
| `CpuTranslatedAddress.QuadPart` | `(LONGLONG)0xFFFFFFFE00000000ull` | | 993 |
| `Size` | `(SIZE_T)BC250_WDDM_APERTURE_BYTES` | `0x10000000` | 994 |
| `CommitLimit` | `(SIZE_T)BC250_WDDM_APERTURE_BYTES` | `0x10000000` | 995 |

Flags word as compiled: `0x00000015`. `Flags.Aperture` is the bit that matters to more than this table: it
is what `VIDMM_GLOBAL::VerifySegmentSet` requires of any segment named as a DMA-buffer or paging-buffer
home, which is rules R34 and R35 and the reason `DmaBufferSegmentSet = 0x2` is legal (facts M66).

The geometry of segment 1 matches what the lab's dxgkrnl read back in E16 run 004: "one CPU-visible local
segment over the carve-out, 0x1FD736000 bytes at GPU 0xF4008CA000" (M65). The host harness reproduces it
from the same code with unit A's geometry compiled in, which is why the numbers here are not hand-typed.
Segment 2 has not been seen by a lab dxgkrnl yet; it is here as the compiled driver declares it.

## (d) DRIVER_INITIALIZATION_DATA

1376 bytes (832 before ADR 0019 B1), 60 DDI pointers set plus `Version` (`wddm.c` `WddmBuildTable`).
`Version = DXGKDDI_INTERFACE_VERSION_WDDM3_1`, the binary compiled at `DXGKDDI_INTERFACE_VERSION 0x10004`
(`bc250kmd.h`); every member after the WDDM 2.0 block (offset 832 on) is NULL and checked so at run time, and
`WDDMVersion` stays `DXGKDDI_WDDMv2`. Stage B and stage C added code
behind these pointers but no pointer: the table is the same 60 it was before the ring path existed, which is
what "no new capability and no new DDI" looks like from dxgkrnl's side.

Carried over from the display-only driver (27): `AddDevice`, `StartDevice`, `StopDevice`, `RemoveDevice`,
`ResetDevice`, `DispatchIoRequest`, `InterruptRoutine`, `DpcRoutine`, `QueryChildRelations`,
`QueryChildStatus`, `QueryDeviceDescriptor`, `SetPowerState`, `Unload`,
`StopDeviceAndReleasePostDisplayOwnership`, `SetPointerPosition`, `SetPointerShape`, `Escape`,
`IsSupportedVidPn`, `RecommendFunctionalVidPn`, `EnumVidPnCofuncModality`, `SetVidPnSourceVisibility`,
`CommitVidPn`, `UpdateActiveVidPnPresentPath`, `RecommendMonitorModes`, `QueryVidPnHWCapability`,
`SystemDisplayEnable`, `SystemDisplayWrite`.

Added for the full table (33): `QueryAdapterInfo`, `GetNodeMetadata`, `CreateDevice`, `DestroyDevice`,
`CreateContext`, `DestroyContext`, `CreateProcess`, `DestroyProcess`, `GetRootPageTableSize`,
`SetRootPageTable`, `CreateAllocation`, `DestroyAllocation`, `DescribeAllocation`,
`GetStandardAllocationDriverData`, `OpenAllocation`, `CloseAllocation`, `BuildPagingBuffer`,
`SubmitCommand`, `SubmitCommandVirtual`, `PreemptCommand`, `ResetFromTimeout`, `RestartFromTimeout`,
`QueryDependentEngineGroup`, `QueryEngineStatus`, `ResetEngine`, `CollectDbgInfo`, `SetStablePowerState`,
`CalibrateGpuClock`, `FormatHistoryBuffer`, `Present`, `SetVidPnSourceAddress`, `ControlInterrupt`,
`GetScanLine`.

Left NULL on purpose, and checked at run time by `WddmCheckReserved()` (`wddm.c`) and at build time by
R18/R19: `DxgkDdiDescribePageTable`, `DxgkDdiUpdatePageTable`, `DxgkDdiUpdatePageDirectory`,
`DxgkDdiMovePageDirectory`, `DxgkDdiSubmitRender`, `DxgkDdiCreateAllocation2`, `Reserved`,
`DxgkDdiSetPowerPState`, `Reserved1`, `Reserved2`, `DxgkDdiAcquireSwizzlingRange`,
`DxgkDdiReleaseSwizzlingRange`. The first four are the trap: they read like the WDDM 2.x page-table DDIs
and are the dead WDDM 1.x ones. Also NULL, and a rule watches each: `DxgkDdiCancelCommand` (R08, no
`CancelCommandAware`), `DxgkDdiMapCpuHostAperture` / `DxgkDdiUnmapCpuHostAperture` (R29, no CPU host
aperture flag). `DxgkDdiPatch` and `DxgkDdiRender` are NULL too, and the harness prints both next to the
`CreateContext` matrix because R37 turns on them.

The host harness asserts all of this against the compiled table, not against the source: 60 pointers, the
reserved members NULL.

## (e) The dependency table

Generated by `python tools\wddm_contract_check\check.py --markdown`. Status is against the tree named at the
top. `n/a` means the declaration is not made, so the obligation does not arise; those rules exist so that
making the declaration later is not free.

| Rule | Declaration | Requires | Source | Status |
|---|---|---|---|---|
| R01 | `SupportPerEngineTDR = 1` | `QueryDependentEngineGroup`, `QueryEngineStatus`, `ResetEngine`, `CollectDbgInfo` (Level Zero) | DXGKRNL `DXGADAPTER::Initialize+0x1505` "does not fill in all of the required DDIs"; LEARN; SAMPLE RosKmdAdapter.cpp:1136 | OK |
| R02 | `SupportDirectFlip = 1` | the scanned-out segment declares `Flags.DirectFlip`; `Present` + `SetVidPnSourceAddress` | DXGKRNL `+0x1e82`, byte +0xAC3 behind the enforcement gate; LEARN wddm-v1-2-driver-enforcement; SAMPLE :1146 with :1243 | OK |
| R03 | `FlipCaps.FlipIndependent = 1` | `SetVidPnSourceAddress` accepts an address DWM did not present; `Present` | DXGKRNL `+0x20b4` "WDDM 1.3 driver must support independent flip."; SAMPLE :1057 | OK |
| R04 | `FlipCaps.FlipOnVSyncMmIo = 1` | `ControlInterrupt` (CRTC_VSYNC) + `GetScanLine` | LEARN wddm-driver-and-feature-caps; a queued flip retires only on a reported VSync (`wddm.c:138-146`); SAMPLE RosKmdGlobal.cpp:285, :341 | OK |
| R05 | a full WDDM table (`DxgkInitialize`) | the installed package writes `UserModeDriverName` | DXGKRNL `DpiGetAdapterInfo` sets adapter+0x621, `Initialize+0x762` returns `STATUS_OBJECT_NAME_NOT_FOUND`; facts M64; SAMPLE Ros.inf:54-58 | OK |
| R06 | interface >= 0x5008 with a render core | `CalibrateGpuClock` **and** `SetStablePowerState`, both non-NULL | DXGKRNL `+0x1e1e`, table pointers at adapter+0x388/+0x3E8; facts M64 | OK |
| R07 | `PreemptionAware = 1` | `MultiEngineAware = 1` | LEARN `_DXGK_VIDSCHCAPS` "the OS will halt the driver initialization process"; DXGKRNL matching message | OK |
| R08 | `CancelCommandAware = 1` | `MultiEngineAware = 1` and `DxgkDdiCancelCommand` | LEARN `_DXGK_VIDSCHCAPS`; SAMPLE RosKmdGlobal.cpp:302 | n/a |
| R09 | `NoDmaPatching = 1` | `PreemptionAware` and `MultiEngineAware` | LEARN `_DXGK_VIDSCHCAPS` | n/a |
| R10 | `MultiEngineAware = 1` | `CreateContext`, `DestroyContext` | LEARN `_DXGK_VIDSCHCAPS` on handle replacement when contexts are unsupported | OK |
| R11 | `GpuMmuSupported = 1` | `GPUMMUCAPS` (13), `PAGETABLELEVELDESC` (14), `GetRootPageTableSize`, `SetRootPageTable`, `CreateProcess`, `DestroyProcess`, `SubmitCommandVirtual`, `GetNodeMetadata` | LEARN mcdm-implementation-guidelines; SAMPLE CosKmdAdapter.cpp:1486, :1502, CosKmdGlobal.cpp:322-329 | OK |
| R12 | `GpuMmuSupported = 1` | `IoMmuSupported` stays 0 | LEARN `_DXGK_VIDMMCAPS` "cannot be set at the same time" | OK |
| R13 | either MMU cap set | `VirtualAddressingSupported = 1` | LEARN `_DXGK_VIDMMCAPS` | OK |
| R14 | a full WDDM 2.x miniport | answers types 1, 6, 10, 11 | LEARN mcdm-implementation-guidelines; DXGKRNL asked 1, 11, 13, 14 in that order | OK |
| R15 | a WDDM 2.x driver | types 2, 4, 5 not answered; `default:` refuses with `STATUS_NOT_SUPPORTED` | LEARN mcdm table "must not be supported" | OK |
| R16 | dxgkrnl asks a started adapter for 15 and 47 | both refused, and the refusal tested not assumed | DXGKRNL `+0x967` "a refusal is tolerated"; facts M64, E16 run 003 | OK |
| R17 | answers `HISTORYBUFFERPRECISION` (10) | `FormatHistoryBuffer` | LEARN mcdm table; SAMPLE CosKmdGlobal.cpp:332 | OK |
| R18 | `NumberOfSwizzlingRanges = 0` | `AcquireSwizzlingRange`, `ReleaseSwizzlingRange` stay NULL | LEARN what-s-new-for-prior-wddm-2-x-versions "Support for swizzling ranges has been removed."; SAMPLE RosKmdGlobal.cpp:265-266 | OK |
| R19 | any table | the ten reserved members stay NULL | LEARN; the WDDM 1.x page-table trap | OK |
| R20 | `WDDMVersion = DXGKDDI_WDDMv2` | `Version = ..._WDDM2_0` and interface >= 0x5000, so `DXGK_DRIVERCAPS` is the 0x240-byte shape | DXGKRNL `GetDriverCapsSizeFromDdiVersion` ">= 0x5011 -> 0x240"; facts M63 measured 576 | OK |
| R21 | any full miniport | `HighestAcceptableAddress` not left low (-1) | LEARN mcdm; SAMPLE RosKmdAdapter.cpp:959 | OK |
| R22 | a declared segment | `Flags.Agp` stays 0 | LEARN "Flags.Agp on a non-AGP aperture -> the adapter fails to initialize" | OK |
| R23 | `PreemptionAware = 1` | granularity above NONE | LEARN gpu-preemption, NONE "may cause the TDR process to repeatedly reset the GPU" | OK (warning rule) |
| R24 | a GpuMmu driver receiving paging buffers | `SubmitCommand` as well as `SubmitCommandVirtual` | LEARN; a paging buffer arrives on `SubmitCommand` with a NULL context | OK |
| R25 | any scheduled miniport | `BuildPagingBuffer`, `PreemptCommand`, `ResetFromTimeout`, `RestartFromTimeout` | LEARN mcdm minimum; a failure costs bugcheck 0x119 | OK |
| R26 | `PageTableLevelCount` | 2..6, and the index/size members described | LEARN `_DXGK_GPUMMUCAPS` "The minimum value is 2"; WDK d3dukmdt.h:212-213 | OK (4) |
| R27 | page directories in local memory | update mode not `CPU_VIRTUAL` | LEARN `_DXGK_PAGETABLEUPDATEMODE` | OK (`GPU_PHYSICAL`) |
| R28 | >= 1 processing node | `GetNodeMetadata`, refusing ordinals above the count | LEARN enumerating-gpu-nodes; SAMPLE RosKmdAdapter.cpp:1350 | OK |
| R29 | `Flags.SupportsCpuHostAperture = 1` | `MapCpuHostAperture`, `UnmapCpuHostAperture` | LEARN; the WDK union `CpuTranslatedAddress \| CpuHostAperture` | n/a |
| R30 | `SupportSmoothRotation = 1` | `UpdateActiveVidPnPresentPath` | SAMPLE RosKmdAdapter.cpp:1130 with its comment | OK |
| R31 | `SupportNonVGA = 1` | `StopDeviceAndReleasePostDisplayOwnership` | SAMPLE RosKmdAdapter.cpp:1125 with its comment | OK |
| R32 | a display+render KMD | the nine VidPN DDIs plus `Present`, `SetVidPnSourceAddress` | LEARN driverentry-of-display-miniport-driver; SAMPLE RosKmdGlobal.cpp:326-344 | OK |
| R33 | `MaxQueuedFlipOnVSync` non-zero | a flip cap that says how the flip is programmed | SAMPLE RosKmdAdapter.cpp:1023-1030 | OK |
| R34 | `PagingBufferSegmentId` names a segment (not 0) | that segment carries `Flags.Aperture` | DXGKRNL (dxgmms2) `VIDMM_GLOBAL::InitDmaPools` turns the id into a set, `VIDMM_DMA_POOL::Init` calls `VerifySegmentSet` with required flags = `Aperture`, else `STATUS_INVALID_PARAMETER` "DMA buffer can only be allocated from an aperture segment."; facts M66 | n/a |
| R35 | CreateContext answers `DmaBufferSegmentSet` | the set is 0, or every bit N-1 names a declared segment N carrying `Flags.Aperture` | DXGKRNL the same `VerifySegmentSet` rule, applied to context DMA buffers; facts M66; SAMPLE CosKmd "Use physical contiguos memory" | OK (`0x2`) |
| R36 | CreateContext answers `AllocationListSize` | the answer depends on `Flags.GdiContext`, because a GDI context needs `AllocationListSize = 256` | LEARN `_DXGK_CONTEXTINFO`; WDK d3dkmddi.h:1546 (`DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT`), :1519 | OK |
| R37 | CreateContext may answer `Caps.NoPatchingRequired = 0` | either the answer is unconditionally 1, or `DxgkDdiPatch` is in the table with non-zero list sizes | LEARN `_DXGK_CONTEXTINFO_CAPS`, dxgkddipatch; DXGKRNL the three `m_bUseGpuVa \|\| ...` asserts at `DXGCONTEXT::Initialize+0x3eb` | **VIOLATION** |
| R38 | at least one node is declared | CreateContext bounds-checks `NodeOrdinal` | LEARN enumerating-gpu-nodes; the same ordinal space as `GetNodeMetadata` | OK |

38 rules, 33 OK, 1 VIOLATION, 4 n/a. The violation is defect D2 in section (g). R36 was the second
violation until 0.7.14 gave the GDI context its allocation list; its unit-test fixture stays, so the fix
cannot quietly come undone.

R35 reads `0x2` rather than `0`: since stage B, `DmaBufferSegmentSet` names segment 2, and segment 2 carries
`Flags.Aperture`, so `VerifySegmentSet` passes it. The rule was not relaxed to allow this - it models
`VerifySegmentSet`, and would fire on `0x1` (the local segment) exactly as it fires in its unit test.

R34 went `n/a` on 2026-09-21 when `PagingBufferSegmentId` was changed from the local segment to 0, which is
the fix for the refusal that ended E16 run 004. It was a warning until the dxgmms2 reading turned the
"both samples do it differently" hunch into a named refusal with a status and a message (facts M66); it is
now a violation rule, and 0.7.4's answer is one of its unit-test fixtures. The rule stays although it is
`n/a` today, because reverting that one line brings the obligation back.

### The CreateContext answer

`DXGK_CONTEXTINFO`, as `Bc250WddmCreateContext` fills it. Confirmed by the host matrix, which calls the real
DDI with every flag combination and reads the whole structure back.

| Member | Source text | Value | Line |
|---|---|---|---|
| `DmaBufferSize` | `PAGE_SIZE` | 4096 | 1283 |
| `DmaBufferSegmentSet` | `g_ApertureOffered ? BC250_WDDM_SEGMENT_SET(BC250_WDDM_SEGMENT_APERTURE) : 0` | `0x2`, or 0 with no aperture | 1289 |
| `DmaBufferPrivateDataSize` | `0` | 0 | 1291 |
| `AllocationListSize` | `pCreateContext->Flags.GdiContext ? DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT : 0` | 256 for a GDI context, 0 otherwise | 1297 |
| `PatchLocationListSize` | `0` | 0 | 1304 |
| `Caps.NoPatchingRequired` | `pCreateContext->Flags.VirtualAddressing ? 1u : 0u` | 1 for a virtual context, 0 otherwise | 1305 |
| `PagingCompanionNodeId` | `BC250_WDDM_NODE_3D` | 0 | 1306 |

Inputs it looks at: `Flags.Value` (logging), `Flags.VirtualAddressing`, `Flags.GdiContext`. `NodeOrdinal` is
bounds-checked, `hDevice` is checked for NULL and for the wrong object type.

Measured on the lab in E16 run 004: input flags 0x5 (`SystemContext | VirtualAddressing`), answer caps 0x1,
dma 4096, segment set 0, lists 0/0 (M65). The host matrix reproduces that for the same input except for the
segment set, which is `0x2` since stage B gave the driver an aperture segment to name; everything else is
byte for byte what the lab saw, which is what makes the other eleven input combinations worth believing.

## (f) How this is checked

Two suites, neither of which needs the lab.

`python tools\wddm_contract_check\check.py` reads `driver/kmd/wddm.c`, `bc250kmd.h`, `bc250kmd.inf` and
`build.ps1`, extracts sections (a) to (d), and evaluates `rules.json` against them. Non-zero exit on a
violation. `--inventory` prints sections (a)-(d) with line numbers, `--markdown` prints section (e),
`--json` for anything downstream. `python -m unittest discover -s tools\wddm_contract_check` runs it against
in-memory fixtures (28 tests): a synthetic miniport that satisfies everything, and twenty copies of it with
one line removed or changed, each asserting that the rule covering that line fires and no other does. A
checker that only ever sees a passing tree proves nothing. Two of those fixtures are positive on purpose -
a paging buffer in a segment that does carry `Flags.Aperture`, and a conditional `NoPatchingRequired` with
`DxgkDdiPatch` present - so that R34 cannot degenerate into "never name a segment" and R37 into "always
answer 1".

`tools\wddm_contract_check\host\run.ps1 -Kits P:\BC-250\toolchain\nuget` compiles `driver/kmd/wddm.c`
unmodified as a user-mode object against the WDK km headers, links it against a stub kernel, and calls the
real DDIs through the function pointers `WddmBuildTable()` produces. No test hook and no refactor in the
driver: the km headers declare kernel services `DECLSPEC_IMPORT`, so supplying `__imp_ExAllocatePool2` and
sixteen more as data slots pointing at stub functions is enough to link. The driver's own neighbours in
`bc250kmd.sys` are stubbed the same way, as ordinary definitions: `GuardConsumeSetting`, the four `Gfx*`
ring entry points and the six `VidMm*` page-table entry points. That is what makes the ring path testable
on a machine with no GPU: `GfxSubmitReady` can answer FALSE, `GfxSubmitIb` can fail, and the fence can
refuse to arrive, on demand. Every
type in section (a) is called with a NULL pointer, a zero-size buffer, one byte short, the exact size, and a
larger buffer, each inside 64 guard bytes before and after, with the payload pre-filled with 0xCC and then
again with 0x55. Each call is asserted for: the expected status, no guard byte touched, no byte written past
`OutputDataSize`, and every returned byte identical across the two fills, which is how a member left
uninitialized is caught. A fault in the driver is turned into a test result by SEH rather than taking the
process down.

`QUERYSEGMENT4` gets the two-pass protocol on top of that: pass 1 with `pSegmentDescriptor` NULL asking only
for the count, pass 2 with the array, and pass 2 again with a `SegmentDescriptorStride` larger than the
descriptor (must work: dxgkrnl chooses the stride and may compile against a newer structure than we do),
smaller than it (must be refused before anything is written), and with `NbSegment` non-zero but
`pSegmentDescriptor` NULL (an error, not a pass-1 request). Both descriptors are checked, not just the
first, and they are walked with the stride the case handed in: the bytes between one descriptor and the next
belong to dxgkrnl when it chooses a larger stride, so they are not read and not required to be written. The
whole matrix is then run a second time with the `EnableVram` gate closed, where the driver has no segment to
declare.

`DxgkDdiCreateContext` has a matrix of its own, because E16 run 004 died one call after it returned: the
eight flag combinations of `SystemContext` / `GdiContext` / `VirtualAddressing`, an engine affinity, a node
ordinal in and out of range, and an `hDevice` that is valid, NULL or a live handle of the wrong object type.
For each, the whole `DXGK_CONTEXTINFO` is recorded and checked: the status, that only `hContext` and
`ContextInfo` were written (measured against a snapshot of the argument taken after the inputs were set, so
the harness cannot mistake its own writes for the driver's), that every `ContextInfo` byte is deterministic
across two fills, that a refused call writes no `ContextInfo` and returns no handle, and the three
consistency rules R35, R36 and R37.

Stage B and stage C brought three more matrices, all on the same handles the context matrix builds:

- **`DxgkDdiBuildPagingBuffer`**, which ADR 0008 point 5 says may not fail (bugcheck 0x119 parameter 1 =
  0x5). Six operations: `UPDATE_PAGE_TABLE` (11), `MAP_APERTURE_SEGMENT` (5), `FILL` (1), `TRANSFER` (2), an
  operation number we do not implement (31), and `UPDATE_PAGE_TABLE` again with a zero-size DMA buffer. Each
  is asserted to return `STATUS_SUCCESS`, to call `VidMmUpdatePageTable` for operation 11 and for nothing
  else, and to leave the DMA buffer and its guard bytes untouched - the page tables are written by the CPU,
  so an empty paging buffer is the right answer and a zero-size one changes nothing.
- **`DxgkDdiGetRootPageTableSize` / `DxgkDdiSetRootPageTable`**: that the answer is a whole level (512
  entries, 4096 bytes) whether one entry or 65536 are asked for, that the byte size covers the entries it
  claims, that the root `SetRootPageTable` recorded is the root the next submission carries to the ring, and
  that a root `VidMmRootPhysical` cannot translate is not used. The root is measured through the next
  submission rather than by reading the driver's private context structure, which the harness has no
  business touching.
- **`DxgkDdiSubmitCommandVirtual`**, ADR 0008 point 5 again (parameter 1 = 0x2), ten cases: the normal
  packet, the fence that has not arrived yet, `GfxSubmitIb` answering `STATUS_DEVICE_BUSY` and
  `STATUS_INSUFFICIENT_RESOURCES`, `GfxSubmitReady` answering FALSE, `DmaBufferSize = 0`, a context that
  never saw `SetRootPageTable`, `hContext` NULL as a paging submission arrives, size 0 and no root at once,
  and a megabyte at a high virtual address. Every one must return `STATUS_SUCCESS`; each is also asserted
  for whether the ring was entered at all, and, when it was, that vmid, root, address and size reached it
  unchanged. All ten pass.

Last, what `WddmStop` leaves behind: every pool block allocated during the run is freed (32 of 32), and no
timer is still armed. Stage C arms a submit timer, and a timer outliving the device fires its DPC into freed
memory.

417 checks on the tree named above, 5 failed, all five the defects in section (g).

Neither suite touches the lab, C:, or anything with a window. Build output goes to
`scratch\build\contract-check`.

## (g) Defects this found

**D1. `QueryAdapterInfo` dereferences a NULL `pOutputData` in its two inline arms.**
The `case DXGKQAITYPE_NUMPOWERCOMPONENTS:` arm (`wddm.c:1184-1185`) and the
`case DXGKQAITYPE_HISTORYBUFFERPRECISION:` arm (`wddm.c:1191-1192`) check `OutputDataSize` and then write
through `pOutputData` without testing it. The four out-of-line handlers all check `|| X == NULL`; these two
do not. Reproduced by the harness as an access violation at IRQL PASSIVE on the host; in the driver it is a
bugcheck inside `DxgkDdiQueryAdapterInfo` during adapter start. Not observed on the lab, because dxgkrnl has
always passed a real buffer.

**D2. `Caps.NoPatchingRequired = 0` with no patch path** (`wddm.c:1304-1305`). `Bc250WddmCreateContext`
answers
`Caps.NoPatchingRequired = pCreateContext->Flags.VirtualAddressing ? 1u : 0u`. A context created without
`VirtualAddressing` is therefore told that dxgkrnl must patch its DMA buffer, while the table carries no
`DxgkDdiPatch` and the answer gives `AllocationListSize = PatchLocationListSize = 0` to patch into. The
dxgmms2 reading puts a number on the second half: the three asserts `m_bUseGpuVa || DmaBufferSize`,
`|| AllocationListSize`, `|| PatchLocationListSize` at `DXGCONTEXT::Initialize+0x3eb` are skipped only
because `m_bUseGpuVa` is 1, so zero list sizes are legal exactly while the context is virtual. Found by both
suites independently (R37, and three cases of the host matrix). Latent: every context this driver has been
asked for so far carried `VirtualAddressing`, because our `GetNodeMetadata` reports `GpuMmuSupported`.

**D3. `Flags.GdiContext` was never read. Fixed in 0.7.14.** `AllocationListSize` was answered as a constant
0, where a GDI context must be given `AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT` (256, WDK
`d3dkmddi.h:1546`). Found by both suites (R36, and three cases of the host matrix) while it was open; it is
listed here because the fixture that caught it is still in the unit tests and the matrix case is still in
the harness, so a regression would be caught rather than rediscovered.

D1 and D2 are reported, not fixed: `driver/` is the lead's.

Nothing else, and in particular nothing in the stage B and stage C code. `BuildPagingBuffer` returned
`STATUS_SUCCESS` on all six operations including the unknown one and the zero-size buffer, touched the
paging buffer in none of them, and called `VidMmUpdatePageTable` for operation 11 and no other.
`SubmitCommandVirtual` returned `STATUS_SUCCESS` on all ten cases, including `GfxSubmitIb` failing twice
over, `GfxSubmitReady` answering FALSE, a zero-size buffer and a context with no page directory; where it
did reach the ring it carried vmid, root, address and size unchanged, and where it did not it wrote nothing
to the ring at all. `SetRootPageTable` recorded the root the next submission used and dropped a root
`VidMmRootPhysical` refused. The pool balanced at 32 allocations and 32 frees once `WddmStop` had run, with
no timer left armed.

Guard bytes were untouched in all 417 checks, nothing was written past `OutputDataSize`, every answer was
byte-identical across the two fill patterns, refused types wrote nothing, the reserved table members were
NULL in the compiled table, `CreateContext` wrote only `hContext` and `ContextInfo` and wrote neither on a
refusal, and the stride and NULL-descriptor variants of `QUERYSEGMENT4` behaved as the protocol requires.

## (h) What this does not check

- **Everything after the submission.** The harness now calls nine DDIs: `QueryAdapterInfo`, `CreateDevice`,
  `CreateProcess`, `CreateContext`, `GetRootPageTableSize`, `SetRootPageTable`, `BuildPagingBuffer` and
  `SubmitCommandVirtual`, plus the destroy counterparts. `CreateAllocation`, `DescribeAllocation`,
  `OpenAllocation`, `Present`, `SetVidPnSourceAddress`, the interrupt path and the timeout path are not
  called; they are where the lab has never reached (M65).
- **The ring itself.** `GfxSubmitIb` and its three neighbours are stubs here, so what is checked is that the
  driver hands the ring the right packet and survives every answer the ring can give - not that the packet
  means anything to the GPU. Whether the hardware runs it is a lab question, and the reason the timeout path
  exists is that on this part there is no GPU reset to fall back on (M53).
- **Why E16 run 004 stopped.** That was answered elsewhere, by a static reading of the lab's own dxgmms2:
  `PagingBufferSegmentId = 1` named a non-aperture segment and `VIDMM_DMA_POOL::Init` refused it (M66). R34
  is that reading turned into a rule. The fix is in the working tree; whether the next step passes is a lab
  question.
- **Values against hardware.** That the segment geometry is the right geometry, that 4 page-table levels of
  9 bits is what the GPU's MMU does, that `AlignmentShift = 2` matches the display controller: all of that
  is hardware truth and belongs in `docs/facts.md`, not here. The checker only asks whether a declaration
  and its obligations agree.
- **The INF as installed.** R05 reads `bc250kmd.inf` and `build.ps1` and concludes that the `-UmdStub`
  package writes `UserModeDriverName`. It does not run the installer or read the software key on a target.
- **Concurrency and IRQL.** One thread, PASSIVE_LEVEL. The spin locks are stubs.
- **Rules nobody wrote down.** The table is as complete as its three sources; a cap whose obligation is
  neither in the dxgkrnl reading, nor on Learn, nor visible in a sample driver is not in it. That is the
  honest limit, and it is the reason each row carries its source rather than a claim.
