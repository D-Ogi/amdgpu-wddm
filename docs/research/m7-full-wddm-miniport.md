# M7: from a display-only miniport to a full WDDM miniport a Vulkan ICD can submit to

Research only, host side. No lab access, nothing installed, no repo file edited. Date 2026-09-21.

## Source legend

Every claim below carries one of these. Anything without a source is marked ASSUMPTION and says what
would settle it.

- `WDK:<file>:<line>` - Windows Kits headers under `<BC250_ROOT>\toolchain\nuget\` (`BC250_ROOT` is
  the workspace root, by default the parent directory of this repository), version 10.0.26100.0.
  Shorthand for the four files used:
  - `DISP` = `microsoft.windows.wdk.x64\c\Include\10.0.26100.0\km\dispmprt.h`
  - `KMDDI` = `microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared\d3dkmddi.h`
  - `HK` = `microsoft.windows.sdk.cpp\c\Include\10.0.26100.0\shared\d3dkmthk.h`
  - `UK` = `microsoft.windows.sdk.cpp\c\Include\10.0.26100.0\shared\d3dukmdt.h`
  - `KD` = `microsoft.windows.sdk.cpp\c\Include\10.0.26100.0\shared\d3dkmdt.h`
- `LEARN:<url>` - Microsoft Learn.
- `MESA:<path>:<line>` - `<BC250_ROOT>\ref\mesa` (shallow sparse clone made for this task, 15 MB) and
  `<BC250_ROOT>\ref\mesa\wddm2-extract\` (the Collabora `wddm2` branch, extracted).
- `OURS:<file>` - our own repo, read only.
- ASSUMPTION - inference. Each one says what would falsify it.

## Corrections to the brief

Four names in the assignment do not exist as given. Carrying them forward would waste a day.

1. `DRIVER_INITIALIZATION_DATA` is in **dispmprt.h**, not `d3dkmddi.h` (`WDK:DISP:2690-3043`).
2. `DXGK_MEMORYMANAGEMENTCAPS`, `DXGK_SCHEDULINGCAPS`, `DXGK_PREEMPTION_CAPS`, `DXGK_MISCCAPS` are
   **member names, not types**. The types are `DXGK_VIDMMCAPS` (`WDDK:KMDDI:2255`), `DXGK_VIDSCHCAPS`
   (`WDK:KMDDI:1994`), `D3DKMDT_PREEMPTION_CAPS` (`WDK:KD:1307`), and `MiscCaps` is an anonymous union
   inline at `WDK:KMDDI:2463-2503`.
3. **There is no `DXGKQAITYPE_NODEMETADATA`.** Node metadata is its own DDI, `DxgkDdiGetNodeMetadata`
   (`WDK:DISP:2846`). The full enum is `WDK:KMDDI:1798-1872` and has no such member. (The *user-mode*
   side does have `KMTQAITYPE_NODEMETADATA = 25`, `WDK:HK:2394` - that is the asymmetry.)
4. `DXGK_QUERYADAPTERINFOTYPE` and the segment descriptors are in **d3dkmddi.h**, not `d3dkmdt.h`.

---

# 1. What dxgkrnl requires of a full WDDM 2.x miniport

## 1.1 The version gate

FACT `WDK:UK:34-54` - the interface-version macros, which set the *shape* of
`DRIVER_INITIALIZATION_DATA` through `#if` blocks:

```
DXGKDDI_INTERFACE_VERSION_WDDM2_0  0x5023      WDDM2_4  0x9006
DXGKDDI_INTERFACE_VERSION_WDDM2_1  0x6003      WDDM2_6  0xB004
DXGKDDI_INTERFACE_VERSION_WDDM2_2  0x700A      WDDM3_0  0xF003 / WDDM3_2 0x11007
```
`WDK:UK:82` defaults the macro to WDDM3_2. FACT `LEARN:.../ddi/dispmprt/ns-dispmprt-_driver_initialization_data`
- "The KMD must set this member to DXGKDDI_INTERFACE_VERSION".

FACT `WDK:KMDDI:2383-2404` - the *cap* enum `DXGK_WDDMVERSION` is a different number space:
`DXGKDDI_WDDMv2 = 0x2000` ... `DXGKDDI_WDDMv3_2 = 0x3200`, `DXGKDDI_WDDM_LATEST = WDDMv3_2`. Note the
enumerator is `DXGKDDI_WDDMv2`, not `..._WDDMv2_0`.

FACT, and this is the sentence that decides the whole question,
`LEARN:.../display/wddm-2-1-features`:
> "Dxgkrnl doesn't use the WDDMVersion cap as a way to determine which features are supported; that
> task is left to other caps or DDI presence. However, if the driver reports WDDM 2.1 support through
> the WDDMVersion cap, Dxgkrnl does validate that the caps or DDIs required by WDDM 2.1 are present and
> fail to create the adapter if they aren't. Inconsistent caps result in failure to create adapter or
> segment."

FACT `LEARN:https://learn.microsoft.com/en-us/windows/whats-new/windows-11-requirements` - the only
current first-party minimum: "Graphics card: Compatible with DirectX 12 or later, with a WDDM 2.0
driver." No Learn page deprecates WDDM 2.0, and none states a higher floor for 24H2/25H2.

### VERDICT: compile at `DXGKDDI_INTERFACE_VERSION_WDDM2_0` and report `WDDMVersion = DXGKDDI_WDDMv2`

Reasons, in order of weight:
- 2.0 is the version that *introduced* GpuMmu, so everything we need exists and nothing we do not need
  gets validated.
- Claiming 2.1 or higher buys validation we must then satisfy, for zero functional gain. FACT: WDDM 2.1
  mandates present batching, driver-store side-by-side install and offer/reclaim
  (`LEARN:.../display/wddm-2-1-features`).
- Compiling at 2.0 makes every 2.1+ member **structurally absent** from the struct rather than a NULL we
  have to defend. That is a materially safer posture for a driver with no debugger-on-demand.

Cost curve if we ever move up: 2.2 adds monitored-fence paging DDIs and hardware-context scheduling
(both optional); 2.4 adds GPU-P/IOMMU and scheduling logs; 2.6 makes `DxgkDdiQueryAdapterInfo` formally
mandatory, adds Collect Diagnostic Info as a *requirement*, and is the first version where
`MiscCaps.ComputeOnly` exists; 3.0+ is opt-in capability only.

One caveat to state plainly: MCDM's implementation guidelines demand `WDDMVersion >= DXGKDDI_WDDMv2_6`
(`LEARN:.../display/mcdm-implementation-guidelines`). That page binds **MCDM/ComputeOnly** drivers only,
which we are not (section 1.4). It is nonetheless the single most useful page Microsoft publishes,
because it is the only one that partitions DDIs into required / conditional / optional / prohibited, and
it is cited throughout below as `[MCDM]`.

## 1.2 The DDI set: what may stay NULL

FACT `LEARN:.../display/wddm-driver-and-feature-caps` legitimises leaving pointers NULL at all:
> "Display-Only | Implement all the Display-specific DDIs and return a null pointer for all the
> Render-specific DDIs | Render-Only | Implement all the Render-specific DDIs and return a null pointer
> for all the Display-specific DDIs"

FACT `LEARN:.../ddi/dispmprt/nf-dispmprt-dxgkinitialize` zero-initialises the whole struct first:
`DRIVER_INITIALIZATION_DATA DriverInitializationData = {'\0'};`

### (a) Carried over from `bc250kmd` unchanged - 42 pointers

FACT, by comparing `KMDDOD_INITIALIZATION_DATA` (`WDK:DISP:3047-3122`) with
`DRIVER_INITIALIZATION_DATA` (`WDK:DISP:2690-3043`) member by member. Same function-pointer types, same
semantics, zero code change (KMDDOD line -> full-KMD line):

`Version` 3048->2691, `AddDevice` 3049->2692, `StartDevice` 3050->2693, `StopDevice` 3051->2694,
`RemoveDevice` 3052->2695, `DispatchIoRequest` 3053->2696, `InterruptRoutine` 3054->2697, `DpcRoutine`
3055->2698, `QueryChildRelations` 3056->2699, `QueryChildStatus` 3057->2700, `QueryDeviceDescriptor`
3058->2701, `SetPowerState` 3059->2702, `NotifyAcpiEvent` 3060->2703, `ResetDevice` 3061->2704, `Unload`
3062->2705, `QueryInterface` 3063->2706, `ControlEtwLogging` 3064->2707, `QueryAdapterInfo` 3065->2709,
`SetPalette` 3066->2721, `SetPointerPosition` 3067->2722, `SetPointerShape` 3068->2723, `Escape`
3069->2726, `CollectDbgInfo` 3070->2727, `IsSupportedVidPn` 3071->2729, `RecommendFunctionalVidPn`
3072->2730, `EnumVidPnCofuncModality` 3073->2731, `SetVidPnSourceVisibility` 3074->2733, `CommitVidPn`
3075->2734, `UpdateActiveVidPnPresentPath` 3076->2735, `RecommendMonitorModes` 3077->2736, `GetScanLine`
3078->2738, `QueryVidPnHWCapability` 3079->2796, `StopDeviceAndReleasePostDisplayOwnership` 3089->2813,
`SystemDisplayEnable` 3094->2818, `SystemDisplayWrite` 3095->2819, `GetChildContainerId` 3100->2826,
`ControlInterrupt` 3105->2740, `SetPowerComponentFState` 3107->2801, `PowerRuntimeControlRequest`
3108->2828, `NotifySurpriseRemoval` 3113->2838, `PowerRuntimeSetDeviceHandle` 3119->2885.

This is the concrete payoff of ADR 0006 point 6 and ADR 0007 point 6 (`OURS:docs/adr/0006`, `0007`):
the split into `pnp.c` / `display.c` was right, and the display half moves over untouched.

**One member is dropped and has no home in the full struct:** `DxgkDdiPresentDisplayOnly`
(`WDK:DISP:3084`). FACT `LEARN:.../display/driverentry-of-display-miniport-driver`: "All of these
functions, except for the DxgkDdiPresentDisplayOnly function, can also be implemented by a full display
miniport driver (KMD)." Its replacement is the pair `DxgkDdiPresent` (`WDK:DISP:2751`) +
`DxgkDdiSetVidPnSourceAddress` (`WDK:DISP:2732`). **That is the single biggest display-side change in
M7**: the DOD is a CPU-blit model, the full KMD is a flip-to-physical-address model.

### (b) New and unavoidable - 24 pointers

| Line (DISP) | DDI | Why |
|---|---|---|
| 2732 | `SetVidPnSourceAddress` | replaces `PresentDisplayOnly`; the flip path |
| 2751 | `Present` | generates present DMA; full-graphics only |
| 2710 / 2747 | `CreateDevice` / `DestroyDevice` | [MCDM] minimum |
| 2711 / 2712 | `CreateAllocation` / `DestroyAllocation` | [MCDM] minimum |
| 2713 | `DescribeAllocation` | [MCDM] minimum |
| 2714 | `GetStandardAllocationDriverData` | [MCDM] minimum; shared primary / GDI surfaces |
| 2748 / 2749 | `OpenAllocation` / `CloseAllocation` | [MCDM] minimum |
| 2765 / 2766 | `CreateContext` / `DestroyContext` | required once `MultiEngineAware = 1` |
| 2720 | `BuildPagingBuffer` | [MCDM] minimum, unconditional |
| 2719 | `PreemptCommand` | [MCDM] minimum, unconditional. Failure bugchecks |
| 2724 / 2725 | `ResetFromTimeout` / `RestartFromTimeout` | TDR, unconditional. Failure bugchecks |
| 2846 | `GetNodeMetadata` | [MCDM] minimum; declares engines |
| 2862 | `CalibrateGpuClock` | [MCDM] minimum |
| 2867 | `FormatHistoryBuffer` | [MCDM] minimum |
| 2874 | `SubmitCommandVirtual` | [MCDM] "If GPU virtual addressing is used" |
| 2875 / 2876 | `SetRootPageTable` / `GetRootPageTableSize` | same |
| 2880 / 2881 | `CreateProcess` / `DestroyProcess` | same |

FACT `LEARN:.../display/mcdm-implementation-guidelines`: "If GPU virtual addressing is used, pointers to
the following functions must also be provided: DxgkDdiCreateProcess, DxgkDdiDestroyProcess,
DxgkDdiGetRootPageTableSize, DxgkDdiSetRootPageTable, DxgkDdiSubmitCommandVirtual."

Total floor: about **66 non-NULL pointers**, of which 42 already exist in `bc250kmd`.

`DxgkDdiQueryAdapterInfo` keeps its signature but its body must grow to answer six types it has never
seen: `DRIVERCAPS`, `QUERYSEGMENT4`, `GPUMMUCAPS`, `PAGETABLELEVELDESC`, `NUMPOWERCOMPONENTS`,
`HISTORYBUFFERPRECISION`.

### (c) Safe to leave NULL from day one

`Patch` 2717 and `Render` 2750 - physical-addressing only (section 3.4). `SubmitCommand` 2718 - see the
caveat below. `QueryCurrentFence` 2728, `RecommendVidPnTopology` 2737, `StopCapture` 2739, all overlay
and MPO members (2741, 2757-2759, 2833, 2857, 2879), `LinkDevice` 2772,
`SetDisplayPrivateDriverFormat` 2773, `RenderKm` 2790, `ControlInterrupt2` 2852,
`QueryDependentEngineGroup`/`QueryEngineStatus`/`ResetEngine` 2806-2808 (iff `SupportPerEngineTDR = 0`),
`CancelCommand` 2821 (iff `CancelCommandAware = 0`), `MapCpuHostAperture`/`UnmapCpuHostAperture`
2877/2878 (iff no CPU host aperture), `SetStablePowerState` 2886, `SetVideoProtectedRegion` 2887, and
everything from `WDK:DISP:2891` to `:3042` (WDDM 2.1 through 3.2, structurally absent if we compile at
2.0).

**Must be zero, not merely NULL** - FACT, Learn marks each "reserved and should be set to zero":
`DescribePageTable` 2779, `UpdatePageTable` 2780, `UpdatePageDirectory` 2781, `MovePageDirectory` 2782,
`SubmitRender` 2784, `CreateAllocation2` 2785, `Reserved` 2795, `SetPowerPState` 2851, `Reserved1` 2883,
`Reserved2` 2884. Note the trap: 2779-2782 look like the WDDM 2.x page-table DDIs and are not - they are
the dead WDDM 1.x ones. The real page-table work is `BuildPagingBuffer` operations.

**`AcquireSwizzlingRange` 2715 / `ReleaseSwizzlingRange` 2716 must be NULL** with
`NumberOfSwizzlingRanges = 0`: FACT `LEARN:.../display/what-s-new-for-prior-wddm-2-x-versions`,
"Support for swizzling ranges has been removed."

**Caveat on `DxgkDdiSubmitCommand` (2718).** [MCDM] files it strictly under "If physical addressing is
used". ASSUMPTION: a pure-GpuMmu driver could leave it NULL. **Do not rely on that** - section 3.5 shows
paging buffers are submitted through `DxgkDdiSubmitCommand` with `hDevice == NULL`, so it is required
anyway. Implement both, routed to the same ring code.

### (d) DDIs where returning a failure code bugchecks - never fail these

FACT, each from its own Learn DDI page; the parameter values from
`LEARN:.../debugger/bug-check-0x119---video-scheduler-internal-error`:

| DDI | Consequence |
|---|---|
| `DxgkDdiPreemptCommand` | "If the driver instead returns an error code, the operating system causes a system bugcheck to occur." Return `STATUS_SUCCESS` unconditionally |
| `DxgkDdiResetFromTimeout` | "otherwise, the operating system bug checks and causes a restart" |
| `DxgkDdiRestartFromTimeout` | same; "can simply return STATUS_SUCCESS immediately" |
| `DxgkDdiSubmitCommand` / `SubmitCommandVirtual` | 0x119 param1 = 0x2 |
| `DxgkDdiPatch` | 0x119 param1 = 0x3 |
| `DxgkDdiBuildPagingBuffer` | Microsoft documents `STATUS_SUCCESS`, `STATUS_GRAPHICS_ALLOCATION_BUSY`, and `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER`. Do not invent other return codes. Correction M186: the0x119 page labels parameter0x5 as a faulted system/paging command; it does not prove the earlier universal mapping of every other builder return to that exact bugcheck. OS-facing failure handling remains open. |
| interrupt-reported fences | 0x119 param1 = 0x1. "The driver must always maintain the last completed fence ID value on the GPU" (`LEARN:.../display/tdr-changes-in-windows-8`) |

`STATUS_NOT_IMPLEMENTED` remains safe in: `DxgkDdiEscape`, `DxgkDdiQueryInterface`,
`DxgkDdiCollectDbgInfo`, `DxgkDdiDispatchIoRequest`, `DxgkDdiSetStablePowerState`, and unhandled
`DXGK_QUERYADAPTERINFOTYPE` values.

This table is the reason M7 cannot use our usual "return an honest failure code" style
(`OURS:docs/adr/0006` point 1) for the scheduler and paging DDIs. Honest failure there is a bugcheck.

## 1.3 The caps that matter

### GPU VA model - GpuMmu, not IoMmu

FACT `WDK:KMDDI:2255-2316`, `DXGK_VIDMMCAPS` flags: `VirtualAddressingSupported` :2269,
`GpuMmuSupported` :2270, `IoMmuSupported` :2271.

FACT `LEARN:.../ddi/d3dkmddi/ns-d3dkmddi-_dxgk_vidmmcaps`: "To express support for GPU virtual memory
addressing, the driver should set the VirtualAddressingSupported cap and GpuMmuSupported or
IoMmuSupported caps. **GpuMmuSupported and IoMmuSupported cannot be set at the same time.**"
(Doc conflict to note: `LEARN:.../display/gpu-virtual-memory-in-wddm-2-0` says "A single GPU node can
support both modes simultaneously". The DDI page is binding.)

**IoMmu is disqualified on a hard technical ground**, not a preference. FACT
`LEARN:.../display/iommu-model`: "both the CPU and GPU share a common address space and CPU page tables.
**Only system memory can be accessed in this case**, so IoMmu is suitable for integrated GPUs." We have
8 GB of local VRAM that must be addressable (fact M31, `OURS:docs/facts.md`).

GpuMmu is also simply what we already have: fact M33 has GART and VM context 0 running under Windows
with AMD's own hub code, and fact M37 has the GPU fetching from system memory through that GART with
AMD's PTE format `0x0003000000000077`. GpuMmu means exposing machinery that already works.

FACT `LEARN:.../display/gpummu-model`: "The hardware format of the page tables used by the GPU MMU is
unknown to VidMm and is abstracted through device driver interfaces (DDIs)." And: "GPU virtual addresses
are managed logically at a fixed 4-KB page granularity through the DDI interface ... memory segments are
managed at either 4 KB or 64 KB at the driver's choice."

Escape hatch if AMD's PTE format fights VidMm's model: `DXGK_GPUMMUCAPS.ExplicitPageTableInvalidation`
(`WDK:KMDDI:2122`), documented for "a software driver that needs to emulate page table".

### Scheduling caps - hard adapter-init gates

FACT `WDK:KMDDI:1994-2051`, `DXGK_VIDSCHCAPS`: `MultiEngineAware` :2000, `VSyncPowerSaveAware` :2001,
`PreemptionAware` :2005, `NoDmaPatching` :2006, `CancelCommandAware` :2007, `No64BitAtomics` :2011,
`LowIrqlPreemptCommand` :2012, `HwQueuePacketCap:4` :2016, `NativeGpuFence` :2020.

FACT `LEARN:.../ddi/d3dkmddi/ns-d3dkmddi-_dxgk_vidschcaps`, three init-time traps:
- "if PreemptionAware is set to 1, the MultiEngineAware member must also be set to a value of 1 ... the
  OS will halt the driver initialization process and return a failure code."
- "when NoDmaPatching is set to 1, the PreemptionAware and MultiEngineAware members must also be set to
  1" - same failure.
- "when CancelCommandAware is set to 1, the MultiEngineAware member must also be set to 1" - same.

Also FACT, same page: "If the driver does not support context creation, for every call to the driver
that would pass a handle to a context, the DirectX graphics kernel subsystem replaces the handle to the
context with a handle to the device." `MultiEngineAware` means "supports contexts", not "has multiple
engines" - we need it set regardless of node count.

### Preemption - claim `DMA_BUFFER_BOUNDARY`, not `NONE`

FACT `WDK:KD:1287-1305`: `D3DKMDT_GRAPHICS_PREEMPTION_GRANULARITY` = NONE 0, DMA_BUFFER_BOUNDARY 100,
PRIMITIVE 200, TRIANGLE 300, PIXEL 400, SHADER 500; `D3DKMDT_COMPUTE_PREEMPTION_GRANULARITY` = NONE 0,
DMA_BUFFER_BOUNDARY 100, DISPATCH 200, THREAD_GROUP 300, THREAD 400, SHADER 500. (There is no
`DXGK_DMA_PREEMPTION_GRANULARITY` type; that name does not exist.)

FACT `LEARN:.../display/gpu-preemption`: "To support the Windows 7 preemption model, set PreemptionAware
to zero" - so `NONE` is accepted. But FACT, same page and the VIDSCHCAPS page: "This may cause the TDR
process to repeatedly reset the GPU which could lead to a system stop error."

FACT `LEARN:.../ddi/d3dkmdt/ne-d3dkmdt-_d3dkmdt_graphics_preemption_granularity`:
`DMA_BUFFER_BOUNDARY` = "The driver cannot stop currently running DMA buffers of a specified type **but
can prevent all pending DMA buffers in the hardware queue from running**."

RECOMMENDATION: `PreemptionAware = 1`, `MultiEngineAware = 1`, `LowIrqlPreemptCommand = 1`, and
`DMA_BUFFER_BOUNDARY` (100) for both graphics and compute. A packet-scheduled driver that owns the ring
can honestly prevent not-yet-started packets from running - by not writing them to the ring. It costs
almost nothing and avoids the TDR-reset-loop failure mode, which on this lab (no reliable warm restart,
fact M21) means a trip for the owner.

### Hardware scheduling is optional - confirmed

FACT [MCDM]: "For optional hardware scheduling support, pointers to the following functions must also be
provided: DxgkDdiCreateHwContext, DxgkDdiCreateHwQueue, ... DxgkDdiSubmitCommandToHwQueue ...".
FACT `LEARN:.../display/querying-wddm-feature-support-and-enablement` - HWSCH is feature ID 0, a
negotiated driver feature, and the sample driver table defaults it to `{FALSE, FALSE, FALSE}`.
FACT - there is an HLK test named "WDDM 2.7 Hardware Scheduling Disabled". The disabled state is a
sanctioned, tested configuration.

**Packet-based scheduling is legal on current Windows.** Leave `CreateHwQueue` (`WDK:DISP:2911`),
`DestroyHwQueue` :2912, `SubmitCommandToHwQueue` :2914, `SwitchToHwContextList` :2915 NULL - or compile
at 2.0 so they do not exist. Note the bucket distinction: `DxgkDdiSubmitCommandVirtual` (GpuMmu,
required) is *not* `DxgkDdiSubmitCommandToHwQueue` (HW scheduling, optional).

### Caps that make dxgkrnl refuse to create the adapter

FACT, consolidated from the pages cited above:

| Cap | Failure |
|---|---|
| `PreemptionAware=1` with `MultiEngineAware=0` | "OS will halt the driver initialization process" |
| `NoDmaPatching=1` without both | same |
| `CancelCommandAware=1` without `MultiEngineAware` | "the OS returns a failure code" |
| `WDDMVersion >= 2.1` with inconsistent DDIs | "fail to create the adapter" |
| `IoMmuSecureModeRequired=1` without IOMMU | "the OS will otherwise not start the adapter" |
| `HighestAcceptableAddress` (`WDK:KMDDI:2409`) too low | [MCDM]: "the load fails" |
| `SupportPerEngineTDR=1` without the 3 engine DDIs | "must implement ..." |
| `Flags.Agp` set on a non-AGP aperture | "the adapter fails to initialize" |

FACT `LEARN:.../display/wddm-v1-2-driver-enforcement`: "If a driver has wrongly claimed itself as WDDM
1.2 or has implemented only some of the mandatory features, then it will fail to create an adapter, and
the system will fall back to the Microsoft Basic Display Driver (MSBDD)." This is the one place Learn
documents a fallback-to-MSBDD path, and it is for *cap inconsistency*, not for a missing UMD.

## 1.4 `DXGK_QUERYADAPTERINFOTYPE` - what must be answered

FACT `WDK:KMDDI:1798-1872`. The ones a minimal full miniport must answer:

| Val | Constant | Required? |
|---|---|---|
| 1 | `DXGKQAITYPE_DRIVERCAPS` | YES [MCDM] |
| 6 | `DXGKQAITYPE_NUMPOWERCOMPONENTS` | YES [MCDM]. Return 0 if no runtime PM |
| 10 | `DXGKQAITYPE_HISTORYBUFFERPRECISION` | YES [MCDM]; pairs with `DxgkDdiFormatHistoryBuffer` |
| 11 | `DXGKQAITYPE_QUERYSEGMENT4` | YES [MCDM] - the memory-segment call for WDDM 2.x |
| 13 | `DXGKQAITYPE_GPUMMUCAPS` | YES under GpuMmu [MCDM] |
| 14 | `DXGKQAITYPE_PAGETABLELEVELDESC` | YES under GpuMmu [MCDM] |
| 2,4,5 | `QUERYSEGMENT`/`2`/`3` | No - [MCDM] "must not be supported" |

Doc conflict, flagged: the enum page marks type 14 "Reserved for system use. Do not use in your driver",
while `LEARN:.../display/gpu-virtual-address` says "Every level is described by the
DXGK_PAGE_TABLE_LEVEL_DESC structure which the KMD fills in during a DxgkDdiQueryAdapterInfo call." The
conceptual page and [MCDM] agree against the enum annotation; treat the annotation as stale.

### Segments

FACT `WDK:KMDDI:2720-2749` `DXGK_SEGMENTDESCRIPTOR4`: `Flags` 2722, `BaseAddress` 2723, `Size` 2724,
`CommitLimit` 2725, union `{CpuTranslatedAddress 2733 | CpuHostAperture 2736}`, `NumInvalidMemoryRanges`
2740, VPR fields 2741-2745, `NumUEFIFrameBufferRanges` 2747. Flags at `WDK:KMDDI:2559`: `Aperture` 2565,
`Agp` 2566, `CpuVisible` 2567, `Use64KBPages` 2578, `SupportsCpuHostAperture` 2580,
`LocalBudgetGroup` 2586.

FACT `LEARN:.../ddi/d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentout4` - **the two-pass protocol**:
> "DxgkDdiQueryAdapterInfo (DXGKQAITYPE_QUERYSEGMENT4) will be called twice. First time, NbSegment will
> be set to 0. The driver should return STATUS_SUCCESS and set NbSegment to the number of GPU memory
> segments in the adapter without accessing any other member of the structure."

and: "The pSegmentDescriptor type has been changed to a BYTE* to help enforce the use of the stride as
the method of iterating the array." Iterate with `SegmentDescriptorStride` (`WDK:KMDDI:2760`), never
`sizeof()`. `PagingBufferSegmentId` is "the index (starting from 1)".

Proposed layout for unit A (ASSUMPTION - consistent with facts M31/M33/M37 but not yet measured through
VidMm):
- **Segment 1, local VRAM, memory segment**: `Aperture = 0`, `CpuVisible = 1`, `LocalBudgetGroup = 1`,
  `BaseAddress` = GPU base of VRAM, `CpuTranslatedAddress = 0x270000000` (fact M31), `Size` shrunk to
  exclude the firmware framebuffer and the region our GART table and PSP TMR occupy (MC `0xF5FF800000`
  upward, facts M33/M34). `Use64KBPages` is worth taking: it cuts page-table pressure on 8 GB
  considerably and `LEARN:.../display/gpummu-model` explicitly permits "either 4 KB or 64 KB at the
  driver's choice".
- **Segment 2, GART aperture** - optional, and **omit it at first**. FACT [MCDM]: "Devices aren't
  required to support a memory aperture." Adding it costs `MAP_APERTURE_SEGMENT`/`UNMAP_APERTURE_SEGMENT`
  in `BuildPagingBuffer` and the dummy-page rule, which is conformance-tested.
- Do **not** set `Flags.Agp`. Our GART is not AGP, and setting it fails adapter init.

FACT: at WDDM 2.0 the field `NumUEFIFrameBufferRanges` does not exist (it is WDDM 2.2+), and
`DXGKQAITYPE_UEFIFRAMEBUFFERRANGES` (18) is the 2.2+ sanctioned way to declare the inherited
framebuffer. **At 2.0 we must simply carve the firmware framebuffer out of the segment we declare.**

### GPU VA space declaration

FACT, three places, none of them the segment descriptor:
1. `DXGK_GPUMMUCAPS` (`WDK:KMDDI:2113-2161`) via type 13: `VirtualAddressBitCount` :2153,
   `PageTableLevelCount` :2155 ("The minimum value is 2"), `PageTableUpdateMode` :2152
   (`DXGK_PAGETABLEUPDATEMODE` at :2055: `CPU_VIRTUAL`, `GPU_VIRTUAL`, `GPU_PHYSICAL`;
   "When page directories are located in a local GPU memory segment, the update mode cannot be set to
   DXGK_PAGETABLEUPDATE_CPU_VIRTUAL"), `LeafPageTableSizeFor64KPagesInBytes` :2154.
2. `DXGK_PAGE_TABLE_LEVEL_DESC` (`WDK:KMDDI:2072-2079`) via type 14, per level.
3. `DXGK_DRIVERCAPS.InternalGpuVirtualAddressRangeStart/End` (`WDK:KMDDI:2452/2453`). FACT [MCDM]: "If
   both the start and end values are zero, the OS will use the entire available VA range." **Set both to
   zero.**

### Engines / nodes

FACT: node *count* is `DXGK_DRIVERCAPS.GpuEngineTopology` (`WDK:KMDDI:2430`) ->
`DXGK_GPUENGINETOPOLOGY.NbAsymetricProcessingNodes` (`WDK:KMDDI:2321`). Per-node description is the DDI
`DxgkDdiGetNodeMetadata` (`WDK:DISP:2846`) filling `DXGK_NODEMETADATA` (`WDK:KD:2004-2019`):
`EngineType`, `FriendlyName[32]`, `Flags`, `GpuMmuSupported`, `IoMmuSupported`.

FACT `WDK:KD:1952-1965` `DXGK_ENGINE_TYPE`: OTHER 0, **3D 1**, VIDEO_DECODE 2, VIDEO_ENCODE 3,
VIDEO_PROCESSING 4, SCENE_ASSEMBLY 5, **COPY 6**, OVERLAY 7, CRYPTO 8, VIDEO_CODEC 9. There is no
"compute" engine type - compute rides on `_3D`.

RECOMMENDATION for first bring-up: declare **one** node, `DXGK_ENGINE_TYPE_3D`, `GpuMmuSupported = TRUE`,
and do paging on it too. That sidesteps `DXGKQAITYPE_PHYSICALADAPTERCAPS` / `PagingNodeIndex` entirely
and halves the interrupt and fence plumbing. Add node 1 = `COPY` (our SDMA, already working per fact
M36) once graphics submission works.

FACT `LEARN:.../display/enumerating-gpu-nodes`: `DxgkDdiGetNodeMetadata` must return
`STATUS_INVALID_PARAMETER` for `NodeOrdinal >= GetNumNodes()`, and "all calls to this function must be
successful" for in-range ordinals. FACT: "WDDM 1.3 and later display miniport drivers (KMDs) must
implement DXGKDDI_GETNODEMETADATA."

---

# 2. What happens with no user-mode Direct3D driver

This is the highest-risk area in M7 and the answer is genuinely not documented. Read this section
carefully before committing to the design.

## 2.1 What `UserModeDriverName` actually does

FACT `LEARN:.../display/loading-an-opengl-installable-client-driver`:
> "UserModeDriverName | REG_SZ | The name of the Direct3D user-mode display driver, **which is required
> for the operation of a Direct3D rendering device** regardless of whether the operating system supports
> an OpenGL ICD."

FACT `LEARN:.../display/loading-a-user-mode-display-driver`: "The **Direct3D runtime** obtains the
user-mode display driver's DLL name from the registry in order to load the user-mode display driver in
the runtime's process space."

FACT: the reader is the D3D runtime in user mode, through `D3DKMTQueryAdapterInfo(KMTQAITYPE_UMDRIVERNAME)`
(`WDK:HK:2364`). FACT `WDK:KMDDI:1798-1872` - **there is no `DXGKQAITYPE_*` by which dxgkrnl asks the
miniport for a UMD name.** UMD load is lazy and user-mode-triggered, not part of adapter start.

FACT `LEARN:.../display/adding-user-mode-display-driver-names-to-the-registry`: `InstalledDisplayDrivers`
is tooling only - "WHQL test programs use the list ... to validate that the driver binaries remain
unchanged over a test run."

Corroborating local evidence: FACT - Microsoft's own KMDOD sample INF
(`<BC250_ROOT>\ref\Windows-driver-samples__WARN-MS-PL-no-code-in-our-driver\video\KMDOD\Sample\sampledisplay.inf`) is `Class=Display` and
declares **no** `UserModeDriverName`, no `InstalledDisplayDrivers`, no `DirectXVersion`. So a
`Class=Display` driver without those values is at least an arrangement Microsoft itself ships.

## 2.2 Does the adapter start? - undocumented, and that absence is the finding

ASSUMPTION (medium-high confidence): a `Class=Display` full miniport with no `UserModeDriverName` starts
with device-manager code 0, dxgkrnl creates the adapter, and only D3D/DXGI device creation fails on it.
Basis: UMD load is lazy (above), and the documented adapter-rejection mechanisms are all about caps/DDI
consistency (section 1.3), never about registry values.

**Nobody has publicly documented deliberately shipping such a driver and reporting the result.** That is
a genuine negative after a thorough search, not a gap in the search.

## 2.3 What DWM does - THE risk

FACT `LEARN:.../windows/compatibility/desktop-window-manager-is-always-on`:
> "If a system does not have a WDDM-compliant graphics driver, Windows 8 uses Microsoft Basic Display
> Adapter as the default adapter. **Since DWM always runs on the default adapter**, it will choose
> Microsoft Basic Display Adapter to compose the desktop when a WDDM-compliant graphics driver is not
> available (whether not installed or disabled) on the system."

FACT `LEARN:.../display/microsoft-basic-display-driver`: "BasicDisplay is always used with BasicRender,
which is the system-supplied module that exposes the functionality of WARP from an adapter in the
kernel." And BasicRender "can also be used on systems that don't have a render-capable driver installed
(for example, display-only devices such as Matrox or DisplayLink that don't have a GPU)."

FACT `LEARN:.../direct3ddxgi/d3d10-graphics-programming-guide-dxgi`: "Starting with Windows 8, an adapter
called the 'Microsoft Basic Render Driver' is always present ... a render-only device that has no display
outputs."

**The gap:** Microsoft documents the WARP fallback for *absent or disabled* drivers. It does **not**
document what DWM does when a WDDM adapter is present, started, is the default adapter, and no D3D
device can be created on it. Because "DWM always runs on the default adapter", the failure mode could be
no desktop rather than a soft fallback.

ASSUMPTION, and the reason the staged plan in section 5 is shaped the way it is: this must be **measured,
not reasoned about**, and measured in a configuration where the machine is recoverable.

ASSUMPTION (high confidence): no bugcheck. A missing UMD is a user-mode DLL load failure. Neither the
0x116 nor the 0x119 documentation mentions UMD absence.

## 2.4 The sentence that argues against us, quoted honestly

FACT `LEARN:.../display/windows-vista-and-later-display-driver-model-architecture` (ms.date 2025-11-05),
under "Third party-supplied modules", verbatim and in this order:
> - "The UMD is a dynamic-link library (DLL) that the Direct3D runtime loads."
> - "The KMD communicates with Dxgkrnl and the graphics hardware."
> - "**A graphics hardware vendor must supply both a UMD and KMD.**"
> - "A third party partner graphics client is a user-mode component that has its own API and framework.
>   **It calls gdi32 thunks to communicate with the kernel-mode graphics subsystem.** The clients that
>   Microsoft is aware of are listed in D3DKMT_CLIENTHINT."

Those two statements are three lines apart and in tension. Microsoft does not reconcile them. The "must
supply both" sentence carries no failure mode and appears in an architecture overview; the
partner-graphics-client bullet is a separate, sanctioned user-mode path (section 3.1).

**Cheapest mitigation, recommended:** ship a stub UMD DLL and point `UserModeDriverName` at it. It costs
a few hundred lines (the D3D10/11 UMD entry point returning `E_NOTIMPL` from `OpenAdapter10_2`), and it
removes the only documented "must" from the argument. Whether it changes DWM's behaviour is exactly the
thing to measure.

## 2.5 The primary-adapter constraint - verified verbatim

FACT `https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs/staging/windows-driver-docs-pr/display/wddm-in-windows-8.md`
(the live Learn page renders truncated):
> "Display-only devices are not allowed as the primary graphics device on client systems."
> "Render-only devices are not allowed as the primary graphics device on client systems."
> "All Windows 8 client systems must have a full graphics WDDM 1.2 device as the primary boot device."

The BC-250 is the only GPU in the box. Neither render-only nor display-only is a documented-legal
topology for it. This argues for a **full graphics** driver (display + render in one miniport), which is
what M7 is, and against the tempting shortcut of declaring ourselves render-only.

Consequence: `MiscCaps.ComputeOnly` (`WDK:KMDDI:2472`) must **not** be set. FACT [MCDM]: setting it
commits us to the entire MCDM prohibited-DDI list - no `CommitVidPn`, no `IsSupportedVidPn`, no
`Present`, no VidPn at all - which destroys both the display path and the Basic Display fallback story.

## 2.6 One PCI function, one driver - CONFIRMED

The project's belief is correct.

FACT `LEARN:.../gettingstarted/device-nodes-and-device-stacks`: "The device stack must have one (and only
one) function driver."

FACT `LEARN:.../ddi/dispmprt/nc-dispmprt-dxgkddi_add_device`: "The display port driver calls
DxgkDdiAddDevice once for each of those PCI functions, at which time the display miniport driver can
indicate that it supports the PCI function (by setting MiniportDeviceContext to a nonzero value) or that
it does not support the PCI function (by setting MiniportDeviceContext to NULL)." Claimed or not
claimed; no partial claim, no second claimant.

FACT: `DxgkDdiLinkDevice` runs the opposite direction - many adapters into one logical adapter - and is
not a way to split one.

FACT: the only documented software display adapter is IddCx, and
`LEARN:.../display/indirect-display-driver-model-overview` says it "uses a user-mode model and doesn't
support kernel-mode components" - a display sink that cannot render, the inverse of what a split would
need.

**So: "display-only KMDOD stays, render goes elsewhere" is not possible for `1002:13FE`.** This also
re-confirms ADR 0007's reasoning for `bc250rd` being a software-only witness rather than a second PnP
driver.

The good news, FACT `LEARN:.../display/seamless-state-transitions-in-wddm-1-2-and-later` and
`.../display/plug-and-play--pnp--start-and-stop-cases`, both with the implementation table
"Full graphics and Display only | Mandatory": the post-display-ownership framebuffer handoff is
**mandatory for full miniports too**, not merely tolerated. Our `display.c` and
`DxgkDdiStopDeviceAndReleasePostDisplayOwnership` carry over and stay correct.

## 2.7 Prior art

| Project | KMD | UMD in INF | Open |
|---|---|---|---|
| viogpudo (virtio-win upstream) | KMDOD only | none | yes |
| viogpu3d (Yttrium fork) | full `DxgkInitialize` | `viogpu_d3d10.dll` | yes, BSD-3 |
| VBoxWddm | both paths, picks DOD when no 3D | `VBoxDispD3D.dll` | yes, GPL |
| VMware `vm3dmp.sys` | full | `vm3dum*_10.dll` | **no** |
| ReactOS | stub dxgkrnl on a fork branch | n/a | yes, unusable |

FACT: `viogpu/viogpudo/driver.cpp#L113` calls `DxgkInitializeDisplayOnlyDriver`; its INF has no
`UserModeDriverName`. The 3D work (PR 943) is unmerged; the Yttrium fork's `viogpu3d/driver.cpp#L162`
calls `DxgkInitialize` and its INF sets `UserModeDriverName = "%11%\viogpu_d3d10.dll"`.

FACT, and this is the strongest prior-art datapoint **against** a UMD-less design: VirtualBox's
`VBoxMPWddm.cpp` has both entry points (`DxgkInitializeDisplayOnlyDriver` at L5378, `DxgkInitialize` at
L5494) and `DriverEntry` **deliberately degrades to KMDOD when 3D/UMD is unavailable**:
`if (!f3DSupported) { ... g_VBoxDisplayOnly = 1; ... "3D is NOT supported by the host, falling back to
display-only mode.." }`. People who had the option chose never to run a full miniport without its UMD.
That is a design choice, not a documented requirement - but it is a vote.

FACT: **upstream Mesa has zero hardware WDDM winsys.** `winsys/amdgpu`, `winsys/radeon`, `winsys/svga`
are all `drm` only; `src/amd/vulkan/winsys` contains only `amdgpu`. FACT:
`src/gallium/frontends/d3d10umd` is a real D3D10 UMD DDI implementation but its `D3DKMT.cpp` **stubs the
thunks** ("so that this can be loaded as a software DLL") and its target hard-wires `driver_swrast`. Not
a model. FACT: Dozen (`dzn_dxcore.cpp`) and gallium d3d12 (`d3d12_screen.cpp`) `util_dl_open("dxcore")` /
`("D3D12.DLL")` - they enumerate *other vendors'* installed D3D12 drivers. Confirmed not a model.

**The closest structural precedent**, and worth reading before writing a line of M7 code:
`github.com/arehnman/virtio-win-mesa`, `src/virtio/vulkan/vn_renderer_d3dkmt.c` - a Mesa Vulkan ICD on
Windows over D3DKMT against an open WDDM miniport (`github.com/arehnman/yttrium-virtio-gpu`, BSD-3). It
calls the exact D3DKMT set we need. Caveat: the backend is virtio to a host GPU, so it validates the
plumbing and not the hardware half. Note their `BUILDING.md` makes the KMD package build *fail* without
the Mesa UMD DLLs even though the headline client is Vulkan.

**The clean negative:** nobody has combined an open WDDM miniport with real GPU silicon. Every open WDDM
KMD found is paravirtual. AMD's public PAL has no Windows OS layer (`pal/src/core/os/` contains only
`amdgpu/` and `nullDevice/`), so AMD's Windows KMD interface cannot be learned from it.

On the BC-250 specifically: three 2026 Windows attempts exist, all single-author, none working, none
independently verified. The one concrete datapoint - `gottmoz/BC-250-Windows-graphics-driver` issue #12,
2026-08-25 - claims the first full-WDDM `DxgkDdiStartDevice` success and reports the adapter **still
rejected post-start with code 43, console black**. Self-reported, no logs, confidence LOW. Treat it as a
hint about where the wall is, not as evidence.

---

# 3. How a Vulkan ICD reaches our KMD

## 3.1 Microsoft sanctions this pattern by name

FACT `WDK:HK:74-105`, `D3DKMT_CLIENTHINT`: `UNKNOWN=0, OPENGL=1, CDD=2, OPENCL=3,` **`VULKAN=4`**
(`WDK:HK:80`)`, CUDA=5, ..., ONEAPI_LEVEL0=19, ...`. FACT: it is an *input* field on
`D3DKMT_CREATECONTEXTVIRTUAL.ClientHint`. The ABI has a dedicated slot for our client.

FACT `LEARN:.../display/providing-kernel-mode-support-to-the-opengl-installable-client-driver`:
> "The OpenGL installable client driver (ICD) can obtain the same level of support for calling
> kernel-mode services as the Direct3D user-mode display driver. However, rather than gaining access to
> kernel-mode services through callback functions ... the OpenGL ICD must load Gdi32.dll and initialize
> use of the OpenGL-kernel-mode-accessing functions"

The page then gives a complete `InitKernelThunks()` doing `LoadLibrary("gdi32.dll")` + `GetProcAddress`
for about 50 D3DKMT entry points by name.

FACT: **none** of the 18 calls we need carries a "reserved for system use" warning on Learn, although
Microsoft does apply that boilerplate elsewhere in the same header (`D3DKMTOpenResource2`,
`D3DKMTGetRuntimeData`, `PFND3DKMT_QUERYSTATISTICS`). The absence is a deliberate editorial choice.

FACT `LEARN:https://learn.microsoft.com/en-us/windows/win32/dxcore/dxcore-enum-adapters` - the single
best citation for this plan:
> "DXCore supports enumeration of MCDM/WDDM devices that don't provide a Direct 3D user mode driver (but
> instead rely on private interfaces or other libraries for interaction)."

FACT: the open proof point is Intel's compute-runtime (OpenCL + Level Zero), fully open **including its
Windows backend**: `shared/source/os_interface/windows/gdi_interface.cpp` resolves ~40 entry points by
`getProcAddress`; `wddm.cpp` uses `CREATECONTEXTVIRTUAL`, `CREATEPAGINGQUEUE`,
`RESERVE/MAP/FREEGPUVIRTUALADDRESS`, `LOCK2`, `EVICT`, `ESCAPE`. It even has a
`um_km_data_translator.*` for the private-data blob contract. That is a complete public model for our
user-mode side, from a vendor, shipping.

Beware one page that looks damning and is not:
`LEARN:/windows/win32/devnotes/-dxgkernel-low-level-client-support` opens with "subject to change ...
Instead, use the DirectDraw and Direct3D APIs" - its function table is entirely `NtGdiDd*` / `D3D8THK.DLL`,
the dead pre-WDDM thunks. Different API surface. Do not let it be cited against the design.

## 3.2 The call set

All `D3DKMT*` are `EXTERN_C NTSTATUS APIENTRY`, exported from **gdi32.dll**, link against `Gdi32.lib`.

### Adapter and device
`D3DKMTEnumAdapters2` `WDK:HK:5995`; `D3DKMTEnumAdapters3` `WDK:HK:6122` (adds
`D3DKMT_ENUMADAPTERS_FILTER` `WDK:HK:2545` with `IncludeComputeOnly`, `IncludeDisplayOnly`);
`D3DKMTOpenAdapterFromLuid` `WDK:HK:5996`; `D3DKMTQueryAdapterInfo` `WDK:HK:5932`;
`D3DKMTCreateDevice` `WDK:HK:5912` (`D3DKMT_CREATEDEVICE` `WDK:HK:49`; its `pCommandBuffer`,
`pAllocationList`, `pPatchLocationList` are marked "D3D10 compatibility" at `WDK:HK:61-66` and are unused
by a GpuMmu ICD); `D3DKMTDestroyDevice` `WDK:HK:5913`; `D3DKMTCloseAdapter` `WDK:HK:5936`.

**Trap, FACT `WDK:HK:2549`:** "ComputeOnly adapters are left out of the default enumeration." If our
adapter were ever classified compute-only it would be invisible to `D3DKMTEnumAdapters2` - and the
Vulkan loader uses `EnumAdapters2`, not `3`. Another reason not to set `MiscCaps.ComputeOnly`.

`KMTQUERYADAPTERINFOTYPE` values we need (`WDK:HK:2361-2474`):
`KMTQAITYPE_UMDRIVERPRIVATE = 0` (:2363) - **our primary caps channel**, serviced by our KMD as
`DXGKQAITYPE_UMDRIVERPRIVATE`; `GETSEGMENTSIZE = 3` (:2366) -> Vulkan heap sizes;
`ADAPTERADDRESS = 6` (:2369); `ADAPTERTYPE = 15` (:2378); `NODEMETADATA = 25` (:2394) - serviced by our
`DxgkDdiGetNodeMetadata`, and how the ICD builds `VkQueueFamilyProperties`;
`QUERY_GPUMMU_CAPS = 34` (:2403) -> `D3DKMT_GPUMMU_CAPS` (`WDK:HK:2167`) whose `VirtualAddressBitCount`
sizes the ICD's VA allocator; `QUERYREGISTRY = 48` (:2423) - used by the Vulkan loader (section 3.3).

FACT `LEARN:.../ddi/d3dkmdt/ns-d3dkmdt-_d3dkmt_wddm_2_0_caps`: `D3DKMT_WDDM_2_0_CAPS` is "Reserved for
system use. Do not use" despite its tempting `GpuMmuSupported` bit. Get GpuMmu per node from
`NODEMETADATA`.

### Context - `CreateContextVirtual`, not `CreateContext`
FACT `LEARN:.../ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsubmitcommand`:
> "D3DKMTSubmitCommand is used to submit command buffers on contexts that support GPU virtual addressing.
> These contexts generate commands directly from user mode, manage their own command buffer pool and
> don't make use of the allocation or patch location list ... **This function replaces the old Render
> function for such contexts and must be used in its place.** Contexts that operate in legacy patch mode
> must continue to use the old Render function."

Rule: `CreateContextVirtual` + `SubmitCommand`, or `CreateContext` + `Render`. Never mix.
`D3DKMTCreateContextVirtual` `WDK:HK:6031`, struct `WDK:HK:5180-5189` - same inputs as `CreateContext`,
but `hContext` is the **only** output.

`D3DDDI_CREATECONTEXTFLAGS` `WDK:UK:672-700`: `NullRendering` 0x1, `DisableGpuTimeout` 0x4,
`HwQueueSupported` 0x10, `TestContext` 0x40. **For bring-up set `DisableGpuTimeout = 1`** - it suppresses
TDR on that context while the command stream is still wrong, which on our lab is the difference between
a retry and a trip for the owner.

FACT `LEARN:.../display/enumerating-gpu-nodes`: `NodeOrdinal` is the node, `EngineAffinity` the engine
within the node. For us, NodeOrdinal is effectively the Vulkan queue family index, `EngineAffinity = 0`.

### Memory
`D3DKMTCreateAllocation2` `WDK:HK:5885`, `D3DKMT_CREATEALLOCATION` `WDK:HK:1608`, per-allocation
`D3DDDI_ALLOCATIONINFO2` `WDK:UK:419-464` - use the `2` forms. Two private blobs reach
`DxgkDdiCreateAllocation`: resource level (`WDK:HK:1624`) and per allocation (`WDK:UK:428`, which is
**"in, and out optional"** - our KMD may write back into it).

FACT `WDK:HK:1557-1568`: `CreateProtected`, `ExistingSysMem`, `CreateWriteCombined`, `CreateCached` are
all "Cannot be used when allocation is created from the user mode". **Cacheability of a UM-created
allocation is therefore decided by our KMD** in `DxgkDdiCreateAllocation` - so the Vulkan memory type
must travel in `pPrivateDriverData` and the KMD sets
`DXGK_ALLOCATIONINFOFLAGS_WDDM2_0.Cached`/`CpuVisible`.

`D3DKMTLock2` / `Unlock2` `WDK:HK:6023/6024` - this is `vkMapMemory`. FACT: no KMD DDI is involved in
GpuMmu (swizzling ranges removed in 2.0); `Lock2` returns a CPU VA for an allocation the KMD marked
`CpuVisible`.

`D3DKMTCreatePagingQueue` `WDK:HK:6021`, `D3DKMT_CREATEPAGINGQUEUE` `WDK:HK:5111` - returns `hPagingQueue`,
`hSyncObject` (a **monitored fence**) and `FenceValueCPUVirtualAddress`. FACT
`LEARN:.../ddi/d3dukmdt/ns-d3dukmdt-d3dddi_mapgpuvirtualaddress`:
> "The user-mode driver must ensure that this fence is retired or explicitly wait on either the CPU or
> the GPU on that fence before allowing the GPU to access the mapped range or an unrecoverable fault
> might occur. A zero fence value might be returned, meaning that the operation is already completed."

Because `hSyncObject` is an ordinary monitored fence there are three ways to consume it: a zero-syscall
poll of `FenceValueCPUVirtualAddress`; a CPU block; or a **GPU-side wait**
(`WaitForSynchronizationObjectFromGpu`), which pipelines the residency dependency instead of stalling
the thread in `vkBindBufferMemory`. The third is the right structure for a Vulkan ICD.

`D3DKMTMakeResident` `WDK:HK:6014` takes `D3DDDI_MAKERESIDENT` (`WDK:UK:1475`). Two traps: `NumAllocations`
is **in/out** - "the number of allocations successfully made resident", so partial success is legal; and
`PriorityList` is "currently ignored and may be set to NULL". FACT `LEARN:.../display/residency-overview`:
"Residency in the WDDM v2 is controlled exclusively by the device residency requirement list ... multiple
calls to MakeResident will require an equal number of Evict calls."

### GPU virtual address - **user mode CAN choose the exact address**

This is the pivotal answer for the RADV port.

FACT `LEARN:.../display/gpummu-model`:
> "The UMD can decide to either **assign a specific GPU virtual address to an allocation**, or let VidMm
> automatically pick an available one, possibly specifying some min and max GPU virtual address
> constraints."

`D3DKMTReserveGpuVirtualAddress` `WDK:HK:6027`, `D3DDDI_RESERVEGPUVIRTUALADDRESS` `WDK:UK:1612`:
FACT `WDK:UK:1619` and Learn - "If BaseAddress is non-NULL, the video memory manager attempts to use this
address ... If the range from BaseAddress to BaseAddress+Size isn't free, the call fails. When BaseAddress
is non-NULL, MinimumAddress and MaximumAddress are ignored."

`D3DKMTMapGpuVirtualAddress` `WDK:HK:6026`, `D3DDDI_MAPGPUVIRTUALADDRESS` `WDK:UK:1595`: `BaseAddress`
`WDK:UK:1598` - "If the range ... isn't free, it must belong to a range previously obtained by calling
pfnReserveGpuVirtualAddressCb or pfnMapGpuVirtualAddressCb". And **`DriverProtection` `WDK:UK:1605`**,
a 64-bit opaque passthrough: FACT Learn - "The specified driver protection will be used in call to
DxgkDdiUpdatePageTable for page table entries corresponding to this virtual address range." **That is a
per-PTE hook straight from the ICD to our page-table code** - the natural home for AMD's VM page flags
(`READABLE`/`WRITEABLE`/`EXECUTABLE`/`MTYPE_UC`/`PRT`), which have no home in the standard
`D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE` (`WDK:UK:1525`: `Write`, `Execute`, `Zero`, `NoAccess`,
`SystemUseOnly`).

Constraints, consolidated (FACT, from the three Learn pages):

| | Reserve | Map | UpdateGpuVirtualAddress |
|---|---|---|---|
| `BaseAddress` alignment | **64 KB** | **4 KB** | 4 KB |
| Size units | **bytes**, multiple of 64 KB | **4 KB pages** (`SizeInPages`) | **bytes** |
| Named range must be | free | free, or inside a prior Reserve/Map | inside one reserved (zero) range |

FACT: the VA space is **per process**, not per device or per context -
`LEARN:.../display/gpu-virtual-memory-in-wddm-2-0`: "each process gets assigned a unique GPU virtual
address (GPUVA) space that every GPU context can execute in ... This assigned GPUVA remains constant and
unique for the lifetime of the allocation."

FACT `WDK:UK:1614-1638`: the Reserve struct **changed meaning** (Learn revised 2024-07-19) and the header
shows the `// (M2)` unions: `hPagingQueue` <-> `hAdapter`, `ReservationType` <-> `Reserved0`,
`DriverProtection` <-> `Reserved1`, `PagingFenceValue` <-> `Reserved2`. Zero the struct, set only
`hAdapter`/`BaseAddress`/`Min`/`Max`/`Size`, and never wait on Reserve's fence slot.

**VERDICT: RADV can keep its own VA allocator.** Reserve 64 KB-aligned windows at chosen bases, then
place allocations inside them with `MapGpuVirtualAddress(BaseAddress = exact)`. One caveat: Learn never
promises `VirtualAddress == BaseAddress`, only that the call **fails** if it cannot honour it - so always
read `VirtualAddress` back.

### Submission
`D3DKMTSubmitCommand` `WDK:HK:6032`, `D3DKMT_SUBMITCOMMAND` `WDK:HK:5200`:
`Commands` (:5202) is a `D3DGPU_VIRTUAL_ADDRESS`; `CommandLength` (:5203) in bytes;
**there is no `hContext` member** - `BroadcastContextCount = 1` and `BroadcastContext[0]` is the target
context (:5206-5207). `NumPrimaries = 0` for offscreen/compute work.

FACT `LEARN:.../ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsubmitcommand`: "This private driver data is
**unidirectional** and the kernel mode driver can't return information to the user mode driver through
this buffer", and its size must be <= what the KMD requested via `DXGK_CONTEXTINFO`, or the call fails.

The command buffer is an allocation **we** created, VA-mapped, map-fence-retired, and made resident.
FACT `LEARN:.../display/access-to-non-resident-allocation`: "Accessing an invalid VA might happen either
because there's no allocation behind the VA or there's a valid allocation but it wasn't made resident" ->
unrecoverable page fault -> engine reset -> adapter-wide TDR.

`D3DKMTSubmitCommandToHwQueue` `WDK:HK:6068` is the hardware-scheduling path. **Do not target it for M7.**

### Synchronization - monitored fences are a natural fit for Vulkan timelines
`D3DKMTCreateSynchronizationObject2` `WDK:HK:5917`;
`D3DDDI_SYNCHRONIZATIONOBJECT_TYPE` `WDK:UK:1384-1407` with **`D3DDDI_MONITORED_FENCE = 5`** (`WDK:UK:1393`).
`MonitoredFence` sub-struct `WDK:UK:1866-1873`:
- `FenceValueCPUVirtualAddress` (:1869) - FACT: "**Read-only** mapping of the fence value (64 bits) for
  the CPU ... The CPU isn't allowed to write to this memory location."
- `FenceValueGPUVirtualAddress` (:1870) - FACT: "**Read/write** mapping ... To signal the fence, the GPU
  is allowed to write directly to this GPU virtual address."

FACT `LEARN:.../display/context-monitoring`: "To signal the fence from the GPU, the UMD inserts a fence
write command in a context command stream **directly without going through kernel mode**."

| Vulkan | WDDM |
|---|---|
| timeline semaphore | monitored fence |
| `vkGetSemaphoreCounterValue` | read `*FenceValueCPUVirtualAddress` - **zero syscalls** |
| `vkSignalSemaphore` | `D3DKMTSignalSynchronizationObjectFromCpu` `WDK:HK:6017` |
| `vkWaitSemaphores` (+WAIT_ANY, +timeout) | `...WaitForSynchronizationObjectFromCpu` `WDK:HK:6016`, `Flags.WaitAny`, `hAsyncEvent` |
| queue signal | GPU write (AMD `RELEASE_MEM`) to `FenceValueGPUVirtualAddress` |
| queue wait | `...WaitForSynchronizationObjectFromGpu` `WDK:HK:6018` |
| `VK_KHR_external_semaphore_win32` | `NtSecuritySharing` + `D3DKMTShareObjects` `WDK:HK:5890` |

FACT `WDK:HK:5028`: `hAsyncEvent` is the **only** way to get a timeout on a CPU fence wait. Wait
comparison is `>=`, not `==`. FACT: `D3DKMTSignalSynchronizationObject`, `...Object2`,
`D3DKMTWaitForSynchronizationObject`, `...Object2` are deprecated - use the `FromCpu`/`FromGpu` forms.

### Escape
`D3DKMTEscape` `WDK:HK:5938`, `D3DKMT_ESCAPE` `WDK:HK:3409`. **`pPrivateDriverData` is bidirectional here**
(`WDK:HK:3415` `// in/out`) - unlike `SubmitCommand`'s. FACT: `D3DKMT_ESCAPE_DRIVERPRIVATE = 0` is the only
type we may use; Learn marks every other type "for testing purposes only". FACT
`LEARN:.../ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_escape`: set `HardwareAccess = 1` for anything touching MMIO -
"If this flag is not set, then the call will fail."

This is the same channel ADR 0007 already uses, and it survives M7 unchanged as the debug/extension path.

## 3.3 ICD registration

Two mechanisms; the loader tries the adapter key first.

**Preferred - the adapter's software key.** FACT, Vulkan-Loader `vk_loader_platform.h:474-511`:
```c
return is_wow ? "Vulkan" VK_VARIANT_REG_STR "DriverNameWow"
              : "Vulkan" VK_VARIANT_REG_STR "DriverName";
```
Exact casing: **`VulkanDriverName`** and **`VulkanDriverNameWow`** (capital W, lowercase `ow`). The
selector is `IsWow64Process`.

FACT, `loader_windows.c:146`: type must be `REG_SZ` or `REG_MULTI_SZ`, else
`VK_ERROR_INCOMPATIBLE_DRIVER`. `REG_MULTI_SZ` = a list of manifests, all loaded.
FACT, Khronos `LoaderDriverInterface.md`: "Each value must be a full absolute path to a JSON manifest
file."

INF form, in the same `AddReg` section that would carry `UserModeDriverName`:
```inf
HKR,, VulkanDriverName,    %REG_MULTI_SZ%, "%13%\bc250_radv64.json"
HKR,, VulkanDriverNameWow, %REG_MULTI_SZ%, "%13%\bc250_radv32.json"   ; omit if no 32-bit ICD
```
`%13%` (DIRID 13, the package's Driver Store directory) expands to an absolute path at install time.
Mechanism source: `LEARN:.../display/adding-software-registry-settings`.

**Microsoft Learn does not document `VulkanDriverName` anywhere.** The only normative spec is the Khronos
doc plus the loader source. Learn documents only the underlying `HKR` software-key mechanism.

**How the loader finds it.** FACT, `loader_windows.c` `windows_read_manifest_from_d3d_adapters`: it
resolves `D3DKMTEnumAdapters2` and `D3DKMTQueryAdapterInfo` from **gdi32.dll**, then for every adapter
issues `QueryAdapterInfo` with type `48` (`KMTQAITYPE_QUERYREGISTRY`, matching `WDK:HK:2423` exactly) and
`LOADER_QUERY_REGISTRY_ADAPTER_KEY = 1`, `REG_MULTI_SZ` first then `REG_SZ`. If any adapter yields a
`VulkanDriverName`, the CfgMgr fallback never runs.

### Does the loader require a D3D-capable adapter? - **NO**

FACT: neither loader path reads `UserModeDriverName`, `InstalledDisplayDrivers`, `KMTQAITYPE_UMDRIVERNAME`
or `KMTQAITYPE_PHYSICALADAPTERDEVICEIDS` - **none of those identifiers appear in the loader at all**. The
only `KMTQAITYPE_*` it uses is `QUERYREGISTRY`.
- Path A needs only that `D3DKMTEnumAdapters2` returns our adapter and that the value can be read off its
  software key. No D3D UMD, no DXGI device creation.
- Path B (CfgMgr) needs only a present, problem-free devnode in the Display class carrying the value.

**Legacy hive path, avoid it.** FACT: `windows_get_registry_files` creates a DXGI factory before reading
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and drops manifests whose filename matches a hardcoded
`known_drivers[]` table (`amd-vulkan64.json`, `amdvlk64.json` -> vendor 0x1002) when no matching DXGI
adapter exists. **Do not name our manifest `amdvlk64.json` or `amd-vulkan64.json`.**

### Bring-up gotchas
FACT: **elevated processes silently ignore `VK_DRIVER_FILES` / `VK_ICD_FILENAMES` / `VK_ADD_DRIVER_FILES`**
(`loader_secure_getenv` -> `is_high_integrity()`), logging "Loader is running with elevated permissions.
Environment variable %s will be ignored". Run `vulkaninfo` from a **non-elevated** shell during bring-up.
FACT: loader dedup is a case-sensitive `strcmp` on the path string, with no canonicalization - registering
the same manifest twice with differently-cased paths loads the DLL twice.
FACT: there is **no signature verification of the ICD DLL anywhere** in the loader. (Our miniport still
needs testsigning; that is separate.)
FACT: a reboot is often needed after INF install - the CfgMgr path skips devnodes with
`CM_PROB_NEED_RESTART`.

## 3.4 `DxgkDdiRender` and `DxgkDdiPatch` - the project's belief is CONFIRMED

FACT, user-mode side, quoted in 3.2: `SubmitCommand` "replaces the old Render function for such contexts
and must be used in its place", and such contexts "don't make use of the allocation or patch location
list".

FACT, kernel side, `LEARN:.../display/residency-overview`:
> "For engines which do support GPU virtual addressing, a new context creation flag
> (DXGK_CONTEXTINFO_NO_PATCHING_REQUIRED) is added ... When this flag is specified, **no patch location
> list will be allocated** and only a very small allocation list (16 entries) will be allocated ... used
> to keep track of write references to primary surfaces and for no other purpose."

| GPU engine | Allocation list | Patch location list |
|---|---|---|
| GPU VA support (`NO_PATCHING_REQUIRED`) | Yes, 16 entries | **No** |
| GPU VA + hardware scheduling | **No** | **No** |

**Verdict: with no UMD, no `D3DKMTRender`, and `DXGK_CONTEXTINFO.Caps.NoPatchingRequired` set,
`DxgkDdiRender` and `DxgkDdiPatch` are never invoked on our render contexts.**

CAVEAT, and worth the twenty lines it costs: `DxgkDdiBuildPagingBuffer`'s page still carries WDDM 1.x-era
text - "Before the video memory manager submits the paging buffer, it calls the driver's DxgkDdiPatch
function to assign physical addresses to the paging buffer; however, in the call to DxgkDdiPatch, the
video memory manager does not provide patch-location lists." Learn never retracts this for GpuMmu.
ASSUMPTION: it no longer applies (GpuMmu paging buffers carry a `DmaBufferGpuVirtualAddress`).
RECOMMENDATION: ship a non-NULL, trivially-succeeding `DxgkDdiPatch` stub. A NULL there that dxgkrnl
calls is 0x119 param1 = 0x3.

## 3.5 The mapping table: user-mode call -> kernel-mode DDI

| D3DKMT call | DDI our miniport must implement |
|---|---|
| `EnumAdapters2/3`, `OpenAdapterFromLuid`, `CloseAdapter`, `CreatePagingQueue` | none (ASSUMPTION - dxgkrnl-internal) |
| `QueryAdapterInfo(UMDRIVERPRIVATE)` | `DXGKDDI_QUERYADAPTERINFO` |
| `QueryAdapterInfo(NODEMETADATA)` | **`DXGKDDI_GETNODEMETADATA`** |
| `QueryAdapterInfo(QUERYREGISTRY / SEGMENTSIZE / ADAPTERTYPE / GPUMMU_CAPS)` | none - dxgkrnl answers from state cached at init |
| `CreateDevice` / `DestroyDevice` | `DXGKDDI_CREATEDEVICE` / `DESTROYDEVICE` |
| first use by a process | **`DXGKDDI_CREATEPROCESS` / `DESTROYPROCESS`** - `hProcess` scopes `UPDATEPAGETABLE` and `FLUSHTLB` |
| `CreateContextVirtual` | `DXGKDDI_CREATECONTEXT` with `Flags.VirtualAddressing = 1`; return `DXGK_CONTEXTINFO.Caps.NoPatchingRequired`. Triggers `GetRootPageTableSize` (if 2 levels) then `SetRootPageTable` |
| `CreateAllocation2` / `DestroyAllocation2` | `DXGKDDI_CREATEALLOCATION` / `DESTROYALLOCATION` (+ `DESCRIBEALLOCATION`) |
| `MakeResident` / `Evict` | `DXGKDDI_BUILDPAGINGBUFFER` -> `DXGKDDI_SUBMITCOMMAND` with **`hDevice == NULL`** |
| `ReserveGpuVirtualAddress` | none - "The address range is only reserved, there is no actual memory behind it" (ASSUMPTION: no PTE writes, so no paging buffer) |
| `MapGpuVirtualAddress` / `FreeGpuVirtualAddress` | `DXGKDDI_BUILDPAGINGBUFFER` `UPDATE_PAGE_TABLE` + `FLUSH_TLB` |
| `UpdateGpuVirtualAddress` | `UPDATE_PAGE_TABLE`, `COPY_PAGE_TABLE_ENTRIES`, `FLUSH_TLB` |
| `Lock2` / `Unlock2` | none in GpuMmu |
| **`SubmitCommand`** | **`DXGKDDI_SUBMITCOMMANDVIRTUAL`** |
| `CreateSynchronizationObject2`, `Signal/WaitFrom{Cpu,Gpu}` | none - dxgkrnl scheduler |
| fence signal from GPU | none - our command buffer writes `FenceValueGPUVirtualAddress` |
| paging-side fence | `DXGKDDI_SIGNALMONITOREDFENCE` |
| `Escape` | `DXGKDDI_ESCAPE` |

FACT `LEARN:.../ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual`, two deltas that matter:
- "STATUS_INVALID_PARAMETER - The DMA or private data is determined to be malformed ... This behavior is
  different from a call to DxgkDdiSubmitCommand, where no error is allowed to be returned due to the
  ability to validate the data in a prior DxgkDdiRender call." **With no UMD this is our only
  command-stream validation hook.**
- "The GPU might have previously worked with a different address space ... The driver is responsible for
  making sure the correct address space is restored ahead of submitting a particular DMA buffer."

`DXGK_BUILDPAGINGBUFFER_OPERATION` - the ones a GpuMmu render driver must handle: **11 `UPDATE_PAGE_TABLE`**,
**12 `FLUSH_TLB`**, **14 `COPY_PAGE_TABLE_ENTRIES`**, **16 `SIGNAL_MONITORED_FENCE`**, plus 8/9
`VIRTUAL_TRANSFER`/`VIRTUAL_FILL` and 5/6 aperture map/unmap if we expose an aperture segment.
ASSUMPTION on 8/9: Learn labels them "WDDMv1 only" but their payload structs are WDDM 2.0 GPU-VA
structures and `DXGK_GPUMMUCAPS.LegacyBehaviors.SourcePageTableVaInTransfer` refers to `TransferVirtual`
during eviction - so in GpuMmu, eviction arrives as `VIRTUAL_TRANSFER`. Implement them.
**Entries 22-25 have no published semantics: the `switch` needs a default that consumes zero DMA bytes
and returns `STATUS_SUCCESS`.**

FACT: `DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE` - "When initializing page tables for the paging process,
the update mode is always DXGK_PAGETABLEUPDATE_CPU_VIRTUAL and pDmaBuffer is set to NULL. In this case
the driver must update page tables immediately."

FACT `LEARN:.../ddi/d3dukmdt/ns-d3dukmdt-_dxgk_pte`: `DXGK_PTE` is VidMm's abstract PTE that our KMD
translates to AMD format - `Valid, Zero, CacheCoherent, ReadOnly, NoExecute, Segment:5, LargePage,
PageTablePageSize:2` + `PageAddress`/`PageTableAddress`. **`Segment == 0` is reserved for system memory** -
do not number local VRAM as segment 0. We already have the AMD-side format from fact M37
(`0x0003000000000077`), so this is a translation function, not a research problem.

---

# 4. What RADV needs from the kernel

## 4.1 The contract

`struct radeon_winsys` is at `MESA:src/amd/vulkan/radv_radeon_winsys.h:228-326`. The structurally
decisive fact: **there is no VA entry point.** GPU VA is an *output field* of `buffer_create`, read via
`struct radeon_winsys_bo::va` (`MESA:...:165`). Everything else follows from that.

Groups: lifecycle (`destroy` :229, `get_fd` :314, `reserve_vmid` :324/:325); queries (`query_value` :231,
`read_registers` :233, `query_gpuvm_fault` :235, `dump_bo_ranges` :308, `dump_bo_log` :310); BO
(`buffer_create` :237, `buffer_destroy` :241, `buffer_map` :242, `buffer_unmap` :255, `buffer_from_ptr`
:244, `buffer_from_fd` :247, `buffer_get_fd` :250, `buffer_get_flags_from_fd` :252,
`buffer_set_metadata` :257, `buffer_get_metadata` :258, `buffer_make_resident` :263, `bo_wait_for_idle`
:312); sparse (`buffer_virtual_bind` :260); contexts (`ctx_create` :265, `ctx_destroy` :266,
`ctx_wait_idle` :268, `ctx_set_pstate` :270); CS (`cs_domain` :272 through `cs_pad` :306, with
**`cs_submit` :288** the core); sync (`get_sync_provider` :316, `copy_sync_payloads` :318).

## 4.2 What the Linux side relies on

**VA: RADV picks addresses in user space. Confirmed.** `MESA:src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c:551-552`:
```c
r = ac_drm_va_range_alloc(ws->dev, amdgpu_gpu_va_range_general, va_size + va_gap_size,
                          virt_alignment, replay_address, &va, &va_handle, va_flags);
```
`amdgpu_va_range_alloc` is **pure user space** - libdrm runs its own allocator over the windows reported
by `AMDGPU_INFO_DEV_INFO`. **No ioctl. The kernel never chooses an address.** Then
`radv_amdgpu_bo_va_op()` (`:33-81`, called at `:647`) programs the page tables via
`DRM_IOCTL_AMDGPU_GEM_VA` with `AMDGPU_VM_PAGE_READABLE|EXECUTABLE`, plus `WRITEABLE` unless read-only
and `AMDGPU_VM_MTYPE_UC` for `RADEON_FLAG_GL2_BYPASS` (`:40-48`).

Ops used: `MAP`, `UNMAP`, `CLEAR`, and `REPLACE` - the atomic "unmap whatever overlaps, then map"
primitive that makes sparse rebinding race-free.

**Submission** is one `DRM_IOCTL_AMDGPU_CS` (`radv_amdgpu_cs.c:1849`) carrying chunks: IB chunks
(`{va_start, ib_bytes, ip_type, ip_instance, ring, flags}`, `:1752-1770`), a FENCE chunk, SYNCOBJ
wait/signal chunks (timeline variants when available), and a **BO list chunk** with `list_handle = ~0`
meaning inline and per-submit (`:1826-1835`). **BO residency is that per-submit handle array.** BOs
created with `AMDGPU_GEM_CREATE_VM_ALWAYS_VALID` are skipped - the kernel tracks them.

**Sync**: RADV does not implement `vk_sync` itself; it hands everything to `vk_drm_syncobj`
(`radv_amdgpu_winsys.c:363`). Binary semaphore = syncobj point 0; timeline semaphore = the same object
with timeline points.

**Info queries**: RADV delegates to `ac_query_gpu_info()` (`MESA:src/amd/common/ac_gpu_info.c:1462-1860`).
The concrete list our KMD must be able to answer is long and specific - `family`, `external_rev`,
`chip_rev`, `device_id`, `ids_flags`, `num_shader_engines`, `num_shader_arrays_per_engine`,
`num_cu_per_sh`, `cu_bitmap[4][4]`, `num_tcc_blocks`, `tcc_disabled_mask`, cache sizes,
`pte_fragment_size`, `gart_page_size`, `virtual_address_max`, `high_va_offset`/`high_va_max`,
`max_engine_clock`, `max_memory_clock`, `enabled_rb_pipes_mask`, `gpu_counter_freq`, `pcie_gen`,
`pcie_num_lanes`, plus `GB_ADDR_CONFIG` via `READ_MMR_REG`, `hw_ip` info per IP
(`available_rings`, `ip_discovery_version`, `ib_start_alignment`, `ib_size_alignment`), firmware
versions for GFX_ME/MEC/PFP (**all three fatal if missing**), and three memory scalars
(`gtt.total_heap_size`, `vram.total_heap_size`, `cpu_accessible_vram.total_heap_size`).
Hard-fail fields: `family` + `external_rev`; an all-zero `cu_bitmap` divides by zero at
`ac_gpu_info.c:1311`.

We are unusually well placed here: facts M7 (firmware versions), M8, M31 (VRAM size and base) and M36
already give us most of these from the hardware itself.

## 4.3 The draft WDDM winsys - and a load-bearing caveat about it

`<BC250_ROOT>\ref\mesa\wddm2-extract\src\amd\vulkan\winsys\wddm2\` - 3043 lines, from
`gitlab.freedesktop.org/lfrb/mesa` branch `wddm2` (Collabora, MR !29945, still Draft).

**CAVEAT 1, important:** FACT `MESA:wddm2-extract/include/drm-uapi/d3dkmthk.h:1-12` is **Microsoft's
dxgkrnl Linux UAPI header** (Copyright 2019 Microsoft, author Iouri Tarassov) carrying `LX_DX*` ioctl
numbers, e.g. `LX_DXMAPGPUVIRTUALADDRESS _IOWR(0x47, 0x0c, ...)` at `:1683`. Collabora was driving AMD's
Windows KMD **through WSL2's `/dev/dxg`**, not native `gdi32.dll` D3DKMT. Hence the `WDDM2_DISPATCH()`
indirection and `#include "wsl/winadapter.h"` at `radv_wddm2_winsys.c:64`. The D3DKMT *semantics* are the
same; the transport is not.

**CAVEAT 2:** it targets an older `struct radeon_winsys` than current main - it assigns `query_info`,
`get_wddm2_handle`, `get_sync_types`, `init_wsi`, `cs_get_cpu_addr` which no longer exist, and changes
the signatures of `buffer_create` and `buffer_virtual_bind`. Porting it forward is not a copy-paste.

**CAVEAT 3, from the Collabora write-up itself** (their blog, 2026-07-28): "Developing our own KMD is not
really an option, so we need to communicate with AMD's." Their entire cost centre was reverse-engineering
AMD's `pPrivateDriverData` blobs. **That constraint is exactly the one we do not have.**

### What it does, and where it gave up

**GPU VA - it did NOT preserve RADV's user-space model.** FACT `MESA:wddm2-extract/.../radv_wddm2_bo.c:501-505`:
```c
   /*bo->base.va = radv_wddm2_bo_va_alloc(ws, flags, phys_size, virt_alignment);
   if (bo->base.va == 0) { result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto error_create; }*/
```
The `util_vma_heap` allocators are initialised (`radv_wddm2_winsys.c:698-701`) and
`radv_wddm2_bo_va_alloc()` exists (`radv_wddm2_bo.c:66-88`) - with **zero live callers**. What actually
runs, `:525-548`, passes `BaseAddress = address` where `address` is RADV's *replay* address, zero for
every normal allocation, so **VidMm picks** and the driver reads back `map.VirtualAddress`. The debugging
detritus left in the source (`.BaseAddress = address, //0, //bo->base.va == 0x100002000 ? 0 : bo->base.va`)
shows they were actively fighting it.

**This is the single most important finding for our design.** Section 3.2 establishes that
`D3DDDI_MAPGPUVIRTUALADDRESS.BaseAddress` *can* take a caller-chosen address; Collabora simply did not
commit to driving it for all allocations. Since we own the KMD, we can honour it unconditionally and keep
RADV's VA model intact.

**Sparse** takes a different path and is the clean part: `ReserveGpuVirtualAddress`, then
`MapGpuVirtualAddress` with `hAllocation == 0` and `Protection.Zero = 1` (`radv_wddm2_bo.c:347-384`) -
**the exact WDDM analogue of `AMDGPU_VA_OP_MAP` + `AMDGPU_VM_PAGE_PRT`**. Rebinding uses
`UpdateGpuVirtualAddress` with `MAP_PROTECT` / `UNMAP`, the `AMDGPU_VA_OP_REPLACE` analogue.

**Submission** is three calls where amdgpu has one atomic ioctl: wait
(`SubmitWaitForSyncObjectsToHwQueue`), submit, signal (`radv_wddm2_cs.c:682-827`). The IB list lives in
`pPrivateDriverData` as a `submit_pdd_gfx_ib` array (`:100-117`) - **structurally identical to amdgpu's
`AMDGPU_CHUNK_ID_IB` array, just relocated into a private blob.**

**Residency was never implemented.** FACT `radv_wddm2_bo.c:53`: `static const bool all_resident = true;` -
every BO is `MakeResident(MustSucceed)` at creation and `Evict` at destroy;
`buffer_make_resident` is a no-op; `cs_add_buffer` populates a set that `cs_submit` never reads
(`radv_wddm2_cs.c:504-508`). `MustSucceed` on every allocation means any real memory pressure fails hard.

**Info queries come from two opaque blobs.** FACT `radv_wddm2_winsys.c:304-305`:
```c
   query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, &props1, 3008);
   query_adapter_info(ws, KMTQAITYPE_UMDRIVERPRIVATE, &props2, 17248);
```
into structs that are pure reverse engineering of AMD's Windows KMD - hundreds of `unk0[550]`,
`unk16[1824]` padding fields with known values pinned at byte offsets. Plus a pile of hardcoded lies:
`hw_ip_version_major = 11` for both GFX and COMPUTE, `*_fw_feature = 29`, `pcie_gen = 4`,
`gart_page_size = 4096`, ray tracing disabled, `ac_fill_tiling_info()` commented out.

**Unfinished:** `buffer_get_handle` returns `false` (no external memory export);
`buffer_get_flags_from_handle` returns a fixed `{VRAM, CPU_ACCESS}` lie; `buffer_set_metadata`,
`bo_wait_for_idle`, `ctx_set_pstate`, `read_registers`, `get_sync_provider`, `copy_sync_payloads` never
assigned; `query_value` defaults to `UNREACHABLE("Unimplemented query")`; only GFX and COMPUTE queues
supported (SDMA asserts); node ordinals come from env vars `RADV_DXGI_3D_NODE`/`RADV_DXGI_COMPUTE_NODE`
with guessed defaults; an unconditional `print_hex_data` on every submit; `GetDeviceState` read before
being filled at `radv_wddm2_cs.c:399-413`.

### The gap table

| Entry point | Linux amdgpu | WDDM (tree B) | Fits GpuMmu? |
|---|---|---|---|
| `buffer_create` | `GEM_CREATE` + `va_range_alloc` + `GEM_VA(MAP)` | `CreateAllocation2` + `MapGpuVirtualAddress` | **PARTLY** - allocation clean, VA not |
| user-space VA | libdrm picks; kernel never chooses | `BaseAddress = 0`, VidMm picks | **PARTLY** - `BaseAddress` *can* be driven; they did not |
| VA flags | `READABLE/WRITEABLE/EXECUTABLE`, `MTYPE_UC` | `Protection.{Write,Zero}` + MTYPE in the alloc blob | **PARTLY** - belongs in `DriverProtection`, per mapping |
| `buffer_map` | `GEM_MMAP` + `mmap()` | `Lock2` | **PARTLY** - `Lock2` has no fixed-address option, so `VK_EXT_map_memory_placed` is unimplementable as written |
| `buffer_from_ptr` | `GEM_USERPTR` | `CreateAllocation2` + `pSystemMem` | YES |
| `buffer_get_fd` | `PRIME_HANDLE_TO_FD` | `return false` | **NO** - export unimplemented |
| `buffer_virtual_bind` | `GEM_VA(REPLACE)` <-> `PAGE_PRT` | `UpdateGpuVirtualAddress MAP_PROTECT/UNMAP` | **PARTLY** - semantics match, but WDDM needs `hContext`+`hFenceObject`+`FenceValue`, so the winsys signature had to change |
| sparse-residency WA (dual VA alias) | two mappings differing in one address bit | not implemented | **NO** under VidMm-chosen VA; **YES** if we honour `BaseAddress` |
| `buffer_make_resident` | global BO list | no-op, `all_resident = true` | **PARTLY** - the DDIs exist, the dynamic model was never wired |
| `ctx_create` | one ctx spans all IPs and rings | `CreateContextVirtual` + `CreateHwQueue` + fence, **per IP** | **PARTLY** - one RADV ctx = N WDDM contexts |
| lost context | submit errno `-ECANCELED`/`-ENODATA`/`-ETIME` | `GetDeviceState(EXECUTION)` -> RESET/HUNG/DMAFAULT | **YES** - arguably better |
| `cs_submit` | one atomic ioctl | three calls (wait, submit, signal) | **PARTLY** - not atomic; a wait can be enqueued and the submit then fail, leaving the queue blocked |
| IB list | `AMDGPU_CHUNK_ID_IB` array | same shape in `pPrivateDriverData` | **YES** - our free hand |
| BO residency | inline per-submit handle array | nothing per-submit | **NO** as implemented |
| binary semaphore | syncobj point 0 | `vk_sync_binary` shim over a monitored fence | YES |
| timeline semaphore | syncobj timeline points | monitored fence 1:1, `FenceValueCPUVirtualAddress` | **YES** - arguably a better fit |
| `read_registers` | `AMDGPU_INFO_READ_MMR_REG` | not implemented | **NO** - needs a private query or escape |
| `ctx_set_pstate`, `reserve_vmid`, `bo_wait_for_idle` | dedicated ioctls | not implemented | **NO** - need private channels |

### The four structural mismatches

1. **VA ownership.** amdgpu: user space owns the address space, the kernel is a PTE programmer. WDDM
   GpuMmu: VidMm owns it and reserves its own regions. Tree B capitulated. We can honour `BaseAddress`
   unconditionally, but must still coexist with whatever VidMm reserves for paging and context state.
2. **Contexts x nodes.** One amdgpu context covers all IPs and rings, selected per IB. A WDDM context is
   bound to one node ordinal. One RADV context becomes N WDDM contexts.
3. **Sync.** The mapping is clean and favours WDDM, *provided* our KMD returns a working
   `FenceValueCPUVirtualAddress` and `FenceValueGPUVirtualAddress`.
4. **Residency.** amdgpu validates a per-submit BO array; WDDM wants `MakeResident`/`Evict` against a
   paging queue. Tree B used neither properly. A real driver implements the paging-queue protocol or
   OOMs under pressure.

## 4.4 The private blobs - our structural advantage

Everything WDDM has no DDI for travels in private data. Collabora recovered each byte-by-byte from AMD's
binary driver. **We define both sides.** Listed by leverage:

1. **Adapter caps blob** (`KMTQAITYPE_UMDRIVERPRIVATE` -> `DXGKQAITYPE_UMDRIVERPRIVATE`). Define it as a
   versioned struct that is literally `struct drm_amdgpu_info_device` + `struct drm_amdgpu_memory_info` +
   `struct drm_amdgpu_info_hw_ip[]` + `GB_ADDR_CONFIG`, and the entire `ac_gpu_info.c` init path works
   unchanged - the nine `ac_fill_*` helpers are non-static with public prototypes at
   `MESA:src/amd/common/ac_gpu_info.h:509-522`. This deletes ~100 lines of `unk[]` padding and the
   WGP->CU `util_widen_mask` reconstruction in one stroke. Biggest single win available.
2. **Allocation-create blob** (`ALLOCATIONINFO2.pPrivateDriverData`). Mirror
   `struct amdgpu_bo_alloc_request` plus the VM flags. This is where `RADEON_FLAG_ZERO_VRAM`,
   `_DISCARDABLE`, `_ENCRYPTED`, `_PREFER_LOCAL_BO` live - all of which tree B silently drops because it
   could not find AMD's bits. Drop tree B's `acc[i%8] += i ^ data[i]` checksum.
3. **Submit blob** (`SubmitCommand.pPrivateDriverData`, one-way). Design it as amdgpu's chunk list: a
   versioned header plus a typed entry array of `(IB VA, byte length, ip_type, ip_instance, ring, flags)`
   matching `struct drm_amdgpu_cs_chunk_ib`. Tree B squashes `ip_type` to one bit and has 4 meaningful
   flag bits; we want the full preamble/postamble/CE/preempt set and a real ring index.
4. **Context-create blob** (`CreateContextVirtual.pPrivateDriverData`). Carry IP type, ring selection,
   priority, register-shadowing/preemption mode, TMZ flag, CSA/shadow buffer VAs.
5. **Per-PTE bits** via `MapGpuVirtualAddress.DriverProtection` (`WDK:UK:1605`). Define it as the amdgpu
   VM page-flag word verbatim. This is the *right* home for MTYPE and the execute bit - tree B wrongly
   routed MTYPE through per-allocation metadata, when both are per-mapping properties. It also gives
   `RADEON_FLAG_VM_PAD_1PAGE` (map the same allocation twice, second read-only) for free.
6. **New channels tree B has none for**: MMIO register reads (`read_registers`, for RGP/SQTT), GPU
   timestamp and sensors, stable pstate, VMID reservation, per-allocation idle wait. `Escape(DRIVERPRIVATE)`
   is bidirectional and is the natural home for all of these - and it is the channel we already have
   working (fact M29).

One detail worth copying from tree B: the HW-queue-create blob has an **out-parameter `queue_id`** the
KMD writes back, referenced later from the submit blob. That bidirectional design is the cleanest part of
the reverse engineering.

---

# 5. A staged M7 that keeps the ADR 0006 failsafe

Constraints this plan is built around: a broken driver must still fall back to Basic Display; the lab
must stay reachable; a hang costs the owner a trip (ADR 0006 context, fact M21); and section 1.2(d) means
we can no longer return honest failures from the scheduler and paging DDIs.

## 5.0 The failsafe, first

**The entry point becomes a gate, not a rewrite.** `DriverEntry` reads `EnableFullWddm` from
`Services\bc250kmd\Parameters` (ADR 0007 point 2: default 0, reset by every install) and calls
`DxgkInitializeDisplayOnlyDriver` when it is 0 and `DxgkInitialize` when it is 1. Both tables are built
from the same function pointers; the display-only table is exactly today's.

Why this shape:
- With the gate closed, the binary is byte-for-byte the driver that is running the owner's display today.
  That is the state after every install, as ADR 0007 already requires.
- The existing `UnconfirmedStarts` budget (`OURS:driver/kmd/README.md`) already refuses to start at 2
  uncleared starts and falls back to Basic Display. It now covers the full-WDDM path for free.
- ASSUMPTION worth stating: I found **no Learn page** describing automatic fallback to `BasicDisplay.sys`
  when a full miniport fails `DxgkInitialize` or `DxgkDdiStartDevice`. **Treat the fallback as not
  guaranteed** and rely on our own guard plus `ErrorControl = 0`, both of which already exist.
- Keep `DxgkDdiStopDeviceAndReleasePostDisplayOwnership` exactly as it is. It is what makes a clean
  handoff back to the firmware framebuffer possible, and it is mandatory for full miniports too
  (section 2.6).

**Two things to verify before the first full-WDDM install**, both cheap and both host-side:
1. That the recovery boot entry and safe mode still work on the current build (they are proven, ADR 0006
   point 5 - just re-confirm).
2. That a bad build cannot be entered twice: the guard increments before anything else in `StartDevice`.

## 5.1 Stage A - the adapter starts and the desktop survives (no submission at all)

The smallest full-WDDM build: `DxgkInitialize` with the 42 carried-over pointers, the 24 new ones
stubbed to the minimum legal behaviour, `WDDMVersion = DXGKDDI_WDDMv2`, one segment (VRAM, carved clear
of the firmware framebuffer), one node (`DXGK_ENGINE_TYPE_3D`), `VirtualAddressingSupported = 1`,
`GpuMmuSupported = 1`, `MultiEngineAware = 1`, `PreemptionAware = 1`, `DMA_BUFFER_BOUNDARY`,
`InternalGpuVirtualAddressRangeStart/End = 0`, `ComputeOnly = 0`, `SupportPerEngineTDR = 0`,
`CancelCommandAware = 0`, `NumberOfSwizzlingRanges = 0`.

Display: `DxgkDdiPresent` and `DxgkDdiSetVidPnSourceAddress` implemented against the inherited firmware
framebuffer. `FlipOnVSyncMmIo` is Mandatory for full graphics
(`LEARN:.../display/wddm-driver-and-feature-caps`), which means a NULL `pDmaBuffer` out of
`DxgkDdiPresent` and the flip done in `SetVidPnSourceAddress`. Since we have exactly one source address
and no real display engine, `SetVidPnSourceAddress` has nothing to program - but it must still be there
and must still report completion.

**This stage answers the question section 2.3 could not.** The measurement is: does the adapter reach
code 0, does the desktop composite, and on what. Evidence to capture: device status, `LastStage`,
`dxdiag`/`DXGI` adapter enumeration, whether DWM is on our adapter or on Microsoft Basic Render Driver,
and a BAR5 sweep against the control to confirm we still touch no register we should not.

Run it **twice**: once with no `UserModeDriverName` at all, once with a stub UMD DLL that fails
`OpenAdapter10_2`. That is a two-hour experiment that settles the single largest unknown in M7, and it
costs one INF value.

Exit criterion: desktop alive at the firmware's mode, device code 0, no bugcheck, uninstall returns Basic
Display on the same framebuffer. If the desktop dies, the gate and the start budget bring the machine
back and we have learned the thing that no public source records.

## 5.2 Stage B - memory and VA, still no submission

Implement, in this order: `DxgkDdiQueryAdapterInfo` for `QUERYSEGMENT4` (two-pass, stride-iterated),
`GPUMMUCAPS`, `PAGETABLELEVELDESC`; `DxgkDdiCreateProcess`/`DestroyProcess`;
`DxgkDdiGetRootPageTableSize`/`SetRootPageTable`; `DxgkDdiCreateAllocation`/`DestroyAllocation`/
`DescribeAllocation`; and **`DxgkDdiBuildPagingBuffer`** for ops 11, 12, 14, 16, 8, 9 with a safe default.

This is where M4's work pays: `gart.c` and the imported `gfxhub_v2_0.c`/`mmhub_v2_0.c` already write AMD
PTEs and already flush TLBs the way amdgpu does (facts M33, M37). `BuildPagingBuffer` becomes a
translation from `DXGK_PTE` to the `0x0003000000000077`-style format we have already validated on
hardware, plus the existing flush sequence.

Drive it from a small host-side tool calling D3DKMT directly - `CreateDevice`, `CreatePagingQueue`,
`CreateAllocation2`, `MapGpuVirtualAddress`, `MakeResident`, `Lock2`, write a pattern, `Unlock2` - and
read the result back through `bc250rd`, which remains the independent witness (ADR 0007 point 5). No
Vulkan, no RADV, no submission. A `plan` mode in the style of `sequence.c` applies here too: log what
would be written to the page table before writing it, and diff against what M4's table already contains.

Exit criterion: an allocation is created, gets the GPU VA **we asked for** (this is the point where the
section 3.2 verdict is tested against real VidMm), is made resident, and its contents are visible at the
expected physical address through the witness.

## 5.3 Stage C - one context, one no-op submission, one fence

The M7 acceptance test. Exact ordered D3DKMT sequence, with the DDI each step exercises:

| # | Call | KMD DDI exercised |
|---|---|---|
| 1 | `D3DKMTEnumAdapters2` -> `OpenAdapterFromLuid` | none |
| 2 | `QueryAdapterInfo(KMTQAITYPE_QUERY_GPUMMU_CAPS)` | none (dxgkrnl, from our `DXGK_GPUMMUCAPS`) |
| 3 | `QueryAdapterInfo(KMTQAITYPE_NODEMETADATA, NodeOrdinal = 0)` | **`DxgkDdiGetNodeMetadata`** |
| 4 | `D3DKMTCreateDevice` | `DxgkDdiCreateDevice` |
| 5 | `D3DKMTCreatePagingQueue` | none |
| 6 | `D3DKMTCreateContextVirtual`, `ClientHint = VULKAN`, `Flags.DisableGpuTimeout = 1` | `DxgkDdiCreateContext` (`Flags.VirtualAddressing = 1`, return `Caps.NoPatchingRequired`); triggers `CreateProcess`, `GetRootPageTableSize`, `SetRootPageTable` |
| 7 | `D3DKMTCreateSynchronizationObject2(D3DDDI_MONITORED_FENCE)` | none; returns `FenceValueCPUVirtualAddress` + `FenceValueGPUVirtualAddress` |
| 8 | `D3DKMTCreateAllocation2` (command buffer, CpuVisible) | `DxgkDdiCreateAllocation` |
| 9 | `Lock2`, write a NOP packet **plus** a `RELEASE_MEM` writing 1 to `FenceValueGPUVirtualAddress`, `Unlock2` | none |
| 10 | `ReserveGpuVirtualAddress` (64 KB-aligned base) then `MapGpuVirtualAddress` (4 KB pages, `DriverProtection` = our PTE bits) | **`BuildPagingBuffer`**(`UPDATE_PAGE_TABLE`, `FLUSH_TLB`) -> `SubmitCommand`(`hDevice == NULL`) -> `SignalMonitoredFence` |
| 11 | wait the paging fence (poll `FenceValueCPUVirtualAddress`) | none |
| 12 | `MakeResident` (check `NumAllocations` on return - partial success is legal) | `BuildPagingBuffer` -> `SubmitCommand`(`hDevice == NULL`) |
| 13 | **`D3DKMTSubmitCommand`**, `BroadcastContextCount = 1`, `BroadcastContext[0] = hContext`, `NumPrimaries = 0` | **`DxgkDdiSubmitCommandVirtual`** -> our ring write and doorbell -> ISR -> `DxgkDdiInterruptRoutine` -> `DxgkDdiDpcRoutine` |
| 14 | `WaitForSynchronizationObjectFromCpu(fence, 1)` | none |
| 15 | sanity: `*(volatile UINT64*)FenceValueCPUVirtualAddress == 1` | **this is the pass/fail** |
| 16 | teardown in reverse | the matching Destroy DDIs |

This depends on M6: the fence write is observed through our IH ring and reported with
`DxgkCbNotifyInterrupt`, and "the driver must always maintain the last completed fence ID value on the
GPU" (section 1.2(d)). Facts M38 and M39 mean the interrupt plumbing and repeatable bring-up are already
in hand.

Everything below the DDI is code we have already run on this unit: the ring, the doorbell and the packet
format are facts M36/M37, where eleven ring tests pass with a `SET_UCONFIG_REG` packet. **Stage C changes
who writes the packet, not whether the packet works.**

Exit criterion for M7's first half: one no-op DMA buffer submitted through `DxgkDdiSubmitCommandVirtual`
completes and its monitored fence reaches 1, observed with zero syscalls from user mode.

## 5.4 Stage D - the ICD

Only after Stage C. Register the ICD through the adapter software key
(`HKR,, VulkanDriverName, %REG_MULTI_SZ%, "%13%\bc250_radv64.json"`), but during bring-up use
`VK_DRIVER_FILES` from a **non-elevated** shell with `VK_LOADER_DEBUG=all` - no signing, no registry, no
reboot. Do not name the manifest `amdvlk64.json` or `amd-vulkan64.json`.

The winsys work is then, in order: the caps blob (section 4.4 item 1, which unlocks `ac_gpu_info.c`
unchanged and is by far the highest-value piece), BO create + VA with `BaseAddress` honoured, monitored
fences, submission. Take the Collabora tree as a **map of the problem, not as a base to fork**: it is on
an old winsys interface, it goes through `/dev/dxg`, and its two hardest decisions (user-space VA,
residency) are the two it abandoned.

## 5.5 Risks, with the mitigation each

| # | Risk | Mitigation |
|---|---|---|
| R1 | DWM has nowhere to go when our adapter is default but has no D3D device | Stage A measures it in both configurations before anything else is built; gate + start budget recover the machine |
| R2 | "All client systems must have a full graphics WDDM device as the primary boot device" - we are the only GPU | Build full graphics, not render-only. Never set `ComputeOnly` |
| R3 | A failure return from `PreemptCommand` / `BuildPagingBuffer` / `ResetFromTimeout` bugchecks | Section 1.2(d) is a checklist; these six return `STATUS_SUCCESS` (or one of `BuildPagingBuffer`'s three legal codes) on every path from the first line of code |
| R4 | `BuildPagingBuffer` ops 22-25 have no published semantics | Default case consumes zero DMA bytes and returns `STATUS_SUCCESS` |
| R5 | Cap inconsistency refuses adapter creation with no diagnostic | Claim WDDM 2.0 and the minimum caps; the section 1.3 table is the pre-flight check |
| R6 | TDR loop from a wrong command stream resets the GPU repeatedly | `Flags.DisableGpuTimeout = 1` on the bring-up context; `TdrTestMode` for controlled tests; claim `DMA_BUFFER_BOUNDARY` so preemption actually works |
| R7 | Segment declaration overlaps the firmware framebuffer, the GART table (MC `0xF5FFE00000`) or the PSP TMR (MC `0xF5FF800000`) | One reservation table in the driver, as already flagged for M5; at WDDM 2.0 the UEFI-framebuffer-range field does not exist, so carve by hand |
| R8 | `MakeResident` partial success silently leaves an allocation non-resident -> page fault -> adapter TDR | Treat `NumAllocations` as in/out on every call |
| R9 | Unit confusion: Reserve = bytes/64 KB, Map = 4 KB pages, Update = bytes | Align everything to 64 KB and assert units at the call site |
| R10 | Private-blob design gets baked in early and is hard to change later | Version every blob from the first byte; the KMD refuses unknown versions with a clear status |

## 5.6 What can be settled host-side, before any lab time

- **A replay/parity test for the caps blob**: build the `UMDRIVERPRIVATE` struct from facts M7, M8, M31,
  M36 and feed it to the real `ac_fill_*` helpers from `<BC250_ROOT>\ref\mesa\src\amd\common\ac_gpu_info.c`,
  then assert the resulting `radeon_info` against what amdgpu reports for this ASIC. Pure host-side, same
  shape as the M4 replay test that scored 285 writes with 0 mismatches.
- **A `DXGK_PTE` -> AMD PTE translation test** against the format already validated in fact M37.
- **A compile-time check** that `DRIVER_INITIALIZATION_DATA` at `DXGKDDI_INTERFACE_VERSION_WDDM2_0` has
  the members we expect and none of the 2.1+ ones, plus a static assert per "must be zero" member.
- **The stub UMD**, which is a few hundred lines and is needed for the Stage A A/B test.

---

# 6. Open questions

| # | Question | How to settle |
|---|---|---|
| O1 | Does a started full WDDM adapter with no `UserModeDriverName` keep the desktop, and does DWM fall back to WARP? | Stage A, run in both configurations. **Undocumented anywhere; this would be a publishable result** |
| O2 | Does dxgkrnl accept a full miniport that declares `NumberOfVideoPresentSources = 0`? | Not needed for our design (we keep the display), but relevant if we ever want a render-only variant. Learn states no minimum in either direction |
| O3 | Does `displib.lib`'s `DxgkInitializeDisplayOnlyDriver` wrap `DxgkInitialize`? | RE hypothesis only, not documented. Irrelevant to the plan but it would explain how cleanly the gate in 5.0 can work |
| O4 | Will VidMm honour `MapGpuVirtualAddress(BaseAddress = exact)` for every allocation, or does it reserve ranges that collide with RADV's fixed heaps? | Stage B. Learn promises only that the call *fails* if it cannot honour the address |
| O5 | Does `DxgkDdiPatch` still get called for paging buffers under GpuMmu? | Ship a succeeding stub and log whether it is ever entered. Settles a documented ambiguity cheaply |
| O6 | Is `DxgkDdiSubmitCommand` genuinely required alongside `SubmitCommandVirtual`? | Yes for paging (`hDevice == NULL`) per Learn; implement both. Log which one dxgkrnl uses for what |
| O7 | Does the Vulkan loader's `EnumAdapters2` see our adapter once it is a full miniport? | Stage D, first thing. Cheap: a four-line host tool |

Nothing here needs a Linux session, so there is nothing to add to `docs/linux-session-wishlist.md` from
this work.

---

# 7. Sources

WDK 10.0.26100.0 headers under `<BC250_ROOT>\toolchain\nuget\` (`dispmprt.h`, `d3dkmddi.h`, `d3dkmthk.h`,
`d3dukmdt.h`, `d3dkmdt.h`), cited by path and line throughout.

Microsoft Learn, the pages that carry weight: WDDM architecture; MCDM KMD implementation guidelines (the
only page that partitions DDIs into required/optional/prohibited); GpuMmu model; IoMmu model; GPU virtual
memory in WDDM 2.0; GPU virtual address; residency overview; context monitoring; gpu-preemption;
tdr-changes-in-windows-8; wddm-2-1-features; wddm-v1-2-driver-enforcement; wddm-driver-and-feature-caps;
enumerating-gpu-nodes; initializing-use-of-memory-segments; seamless-state-transitions;
plug-and-play-start-and-stop-cases; loading-a-user-mode-display-driver;
adding-software-registry-settings; dxcore-enum-adapters; desktop-window-manager-is-always-on;
microsoft-basic-display-driver; bug checks 0x116 and 0x119; plus the individual DDI and struct reference
pages named inline. `wddm-in-windows-8.md` was read from the raw docs repo because the Learn page renders
truncated.

Mesa: `<BC250_ROOT>\ref\mesa` (shallow sparse clone, `origin/main` at `3ae3d2e`) and
`<BC250_ROOT>\ref\mesa\wddm2-extract\` (branch `wddm2` from `gitlab.freedesktop.org/lfrb/mesa`, extracted
with `git archive`). Neither is a source of facts about our hardware; both are read as code.

Prior art repositories read (not cloned): virtio-win/kvm-guest-drivers-windows, arehnman/yttrium-virtio-gpu,
arehnman/virtio-win-mesa, VirtualBox/virtualbox, vmware/open-vm-tools, reactos/reactos,
GPUOpen-Drivers/pal, KhronosGroup/Vulkan-Loader, intel/compute-runtime.

Local reference: `<BC250_ROOT>\ref\Windows-driver-samples__WARN-MS-PL-no-code-in-our-driver` (KMDOD sample and its INF).

Our own repo, read only: `docs/adr/0005`, `0006`, `0007`; `driver/kmd/README.md`;
`docs/00-goal-and-roadmap.md`; `docs/facts.md` (M7, M8, M21, M29, M31, M33, M34, M36, M37, M38, M39).
