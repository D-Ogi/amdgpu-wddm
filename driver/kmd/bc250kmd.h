// bc250kmd: WDDM miniport for the ASRock BC-250 (AMD Cyan Skillfish, PCI 1002:13FE).
// Milestone M3 (ADR 0006): display-only, keeps the firmware's framebuffer. MMIO is a separate, gated part
// (ADR 0007, mmio.c) that the display path never uses.
//
// Layout: entry.c (DriverEntry, the gate and the two DDI tables), pnp.c (device life cycle, children, power),
// display.c (VidPN, present, pointer, system display), guard.c (boot-loop guard, breadcrumbs, log),
// mmio.c (gated register access for the bring-up experiments), vram.c (the VRAM carve-out),
// sequence.c (kernel backend of driver/shim), gart.c (M4), psp.c (M5: firmware through the PSP),
// wddm.c (M7 stage A: the full WDDM table behind the EnableFullWddm gate, ADR 0008).
#pragma once

// M7, ADR 0008 point 2: the whole binary is compiled at the WDDM 2.0 DDI interface version, which is the version
// the full table is declared with. The display-only table keeps telling dxgkrnl DXGKDDI_INTERFACE_VERSION_WIN8,
// exactly as it does today; that value is a run-time field, not this macro.
//
// One version for one binary, not one per translation unit: this macro changes the shape of DXGKRNL_INTERFACE and
// of most DXGKARG_* structures, and BC250_DEVICE carries a DXGKRNL_INTERFACE, so two translation units compiled at
// two versions would disagree about the layout of our own device structure.
//
// That this costs the display-only path nothing was measured, not assumed (P:\BC-250\scratch\m7-stagea\probe.c,
// which emits every size and member offset the display path touches and compares the compilations): of 99 values,
// 94 are identical between the header's default (WDDM 3.2) and 2.0. The five that differ are the total sizes of
// DXGKRNL_INTERFACE, DXGK_DRIVERCAPS, DXGKARG_QUERYADAPTERINFO, DXGKARG_ESCAPE and D3DKMDT_VIDPN_TARGET_MODE; in
// each of them every member this driver reads or writes keeps its offset, and every member that disappears was
// added at WDDM 2.2 or later, which dxgkrnl never hands to a driver declaring WIN8.
//
// KMDDOD_INITIALIZATION_DATA is **not** version-independent, and it matters that the reason it is safe is written
// down rather than the size being quoted. Its one guard is at >= WDDM2_0 (dispmprt.h:3118-3120, which appends
// DxgkDdiPowerRuntimeSetDeviceHandle: measured 0x148 bytes below that version and 0x150 at it and above), so the
// move from 3.2 to 2.0 happens to leave it alone - both sides are >= 2.0. The argument that survives the next
// version bump is a different one: the member is **appended**, the buffer is ours and zeroed before use, and
// dxgkrnl dispatches on the run-time data.Version field, which this driver still sets to WIN8; it therefore reads
// only the prefix that WIN8 defines and never the tail. A member inserted in the middle would break that, which
// is why probe.ps1 now reports this structure's size at three versions and not only at two.
//
// The same probe names the other six that move with the version, and each has its own argument. Two are buffers
// **dxgkrnl** owns and sizes for the version the driver declared, so the rule there is the opposite one - write
// only inside the WIN8 prefix and never assign the whole structure: DXGK_CHILD_STATUS (0x0C at WIN8, 0x10 from
// WDDM 1.3 on; see pnp.c's Bc250QueryChildStatus) and DXGKRNL_INTERFACE (0x100 at WIN8, 0x138 at 2.0), which
// pnp.c already copies with min(DxgkInterface->Size, sizeof(device->Dxgk)) for exactly this reason.
// The literal is what the preprocessor needs here; the assert below binds it to the header's own constant.
#define DXGKDDI_INTERFACE_VERSION 0x5023

#include <ntifs.h>        // superset of ntddk.h; the token checks of the escape need it
#include <windef.h>
#include <dispmprt.h>

C_ASSERT(DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_WDDM2_0);

#define BC250_TAG 'dK52'
#define BC250_CHILD_UID 0x250001        // the one DisplayPort output, as far as this driver is concerned
#define BC250_MAX_UNCONFIRMED_STARTS 2  // guard.c: refuse to start after this many starts nobody confirmed

// ---- VRAM reservations ---------------------------------------------------------------------------------------
// The one table of what the top of the VRAM carve-out (facts M31) is already used for. Every bring-up file places
// its buffers this far below the end of VRAM and takes the distance from here, so that the numbers exist once:
// M7's memory segment (wddm.c) is the part of VRAM that is left over, between the firmware's framebuffer at the
// bottom and BC250_VRAM_TOP_RESERVED at the top.
#define BC250_VRAM_GART_BELOW   0x200000ull     // gart.c: page table (1 MB) and scratch page, MC 0xF5FFE00000 (M33)
#define BC250_VRAM_PSP_BELOW    0x800000ull     // psp.c: TMR (4 MB, MC 0xF5FF800000, M34), staging, the ring pages
#define BC250_VRAM_POOL_BELOW  0x2000000ull     // gpumem.c: the 24 MB pool for rings, MQDs and write-back slots
#define BC250_VRAM_TOP_RESERVED BC250_VRAM_POOL_BELOW       // the whole reserved tail: the largest of the three

// Breadcrumbs: the last value written survives a hang and a power cycle (guard.c). Append only: tools that
// read them from the registry rely on the numbers. The list is mirrored in KmdStages.All
// (tools/win/bc250mon/src/KmdProvider.cs), which names the stages on the overlay; the two must stay in sync
// and tools/win/bc250mon/test_stages.py fails the monitor's build when they do not.
typedef enum _BC250_STAGE {
    StageNone = 0,
    StageDriverEntry = 10,
    StageAddDevice = 20,
    StageStartEnter = 30,
    StageStartGuardPassed = 31,
    StageStartDeviceInfo = 32,
    StageStartPostDisplayAcquired = 33,
    StageStartFramebufferMapped = 34,
    StageStartMmioDone = 35,
    StageStartDone = 39,
    StageFirstCommitVidPn = 50,
    StageFirstPresent = 60,
    StageFirstPresentDone = 61,
    StageStopEnter = 70,
    StageStopDone = 79,
    StageRefusedByGuard = 90,
    StageStartFailed = 91,
} BC250_STAGE;

typedef struct _BC250_DEVICE {
    PDEVICE_OBJECT PhysicalDeviceObject;
    DXGKRNL_INTERFACE Dxgk;
    DXGK_START_INFO StartInfo;
    DXGK_DEVICE_INFO DeviceInfo;

    BOOLEAN Started;
    BOOLEAN SourceVisible;
    BOOLEAN ModeActive;         // a commit has validated that the source surface is exactly the firmware's mode
    BOOLEAN PresentSeen;
    BOOLEAN CommitSeen;

    // The mode the firmware left. M3 offers exactly this one.
    DXGK_DISPLAY_INFORMATION Post;
    PVOID Framebuffer;                  // mapping of Post.PhysicAddress
    SIZE_T FramebufferLength;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;

    // BAR5, NULL unless the EnableMmio gate was open at start (mmio.c).
    volatile ULONG* Mmio;
    PHYSICAL_ADDRESS MmioPhysical;
    BOOLEAN MmioWriteEnabled;
    BOOLEAN MmioGartEnabled;
    BOOLEAN MmioPspEnabled;
    BOOLEAN MmioGfxEnabled;

    // The VRAM carve-out, all zero unless the EnableVram gate was open at start (vram.c).
    BOOLEAN VramEnabled;
    BOOLEAN VramWriteEnabled;
    PHYSICAL_ADDRESS VramPhysical;      // system physical address of VRAM byte 0
    ULONGLONG VramLength;
    ULONGLONG VramMcBase;               // GPU physical (MC) address of VRAM byte 0
    PHYSICAL_ADDRESS Bar0Physical;
    ULONGLONG Bar0Length;

    PVOID Gart;                         // gart.c, NULL unless the EnableGart gate was open at start
    PVOID Psp;                          // psp.c, NULL unless the EnablePsp gate was open at start
    PVOID GpuMem;                       // gpumem.c, NULL unless the EnableGfx gate was open at start
    PVOID Gfx;                          // gfx.c, the same
    PVOID Ih;                           // ih.c, NULL unless the EnableIh gate was open at start
    BOOLEAN FullWddm;                   // DriverEntry found EnableFullWddm open and gave dxgkrnl the full table
    PVOID Wddm;                         // wddm.c, NULL unless FullWddm
    BOOLEAN MmioIhEnabled;
    BOOLEAN IhQuiet;                    // ih.c's stop: the IH ring reads disabled (or never was enabled); for gpumem.c's stop
    volatile LONG InterruptCount;       // every call of the interrupt routine since start, ours or not
    volatile LONG LastMessageNumber;
    BOOLEAN InterruptIsMessage;         // what Windows assigned (pnp.c, from the translated resources)
    ULONG InterruptVector;
    FAST_MUTEX GartLock;                // serializes every bring-up sequence (gart.c, psp.c, gfx.c) and the stop;
                                        // initialized in AddDevice
} BC250_DEVICE;

// One run of a bring-up sequence through the shim: what adev->backend points at (sequence.c).
typedef struct _BC250_ESCAPE_WRITE BC250_SEQUENCE_WRITE;
typedef struct _BC250_SEQUENCE {
    const char* Name;                   // for the log
    NTSTATUS (*Read)(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);     // the sequence's own table
    NTSTATUS (*Write)(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);
    // Optional. PLAN: answer a read instead of performing it (registers whose read has a side effect).
    BOOLEAN (*PlanAnswers)(_In_ struct _BC250_SEQUENCE* Sequence, ULONG DwordIndex, _Out_ ULONG* Value);
    // Optional. A write that must reach the hardware even after the sequence was stopped by a fault.
    BOOLEAN (*PassesFault)(_In_ struct _BC250_SEQUENCE* Sequence, ULONG DwordIndex, ULONG Value);
    PVOID Owner;
    BOOLEAN Dpc;                        // the sequence of ih.c's DPC: no log, no list, nothing shared with the escapes

    BC250_DEVICE* Device;               // set by SequenceBegin
    BOOLEAN Plan;
    NTSTATUS Fault;                     // first refused register access of the running sequence
    ULONG FaultOffset;
    ULONG WriteCount;
    BC250_SEQUENCE_WRITE* Writes;       // where the writes are listed, may be NULL
    ULONG MaxWrites;
} BC250_SEQUENCE;

// guard.c
NTSTATUS GuardInit(_In_ PUNICODE_STRING RegistryPath);
void GuardCleanup(void);
void GuardStage(BC250_STAGE Stage);
BC250_STAGE GuardLastStage(void);
NTSTATUS GuardCheckAndCountStart(void);         // STATUS_SUCCESS, or a failure when the start budget is used up
void GuardLog(_In_z_ const char* Format, ...);                  // DbgPrintEx and the log ring; IRQL <= DISPATCH_LEVEL
ULONG GuardReadSetting(_In_z_ PCWSTR Name, ULONG Default);     // REG_DWORD under Parameters, PASSIVE_LEVEL
ULONG GuardConsumeSetting(_In_z_ PCWSTR Name, ULONG Default);  // the same, and a value of 1 is written back as 0

// The log ring, read back through BC250_ESCAPE_GET_LOG. BC250_LOG_LINE comes from bc250kmd_escape.h, which only
// the two files that touch the ring include; a forward declaration keeps it out of everybody else's way.
struct _BC250_LOG_LINE;
ULONG GuardLogSequence(void);                                   // the sequence number the next line will get
void GuardLogStats(_Out_ ULONG* Total, _Out_ ULONG* Lost, _Out_ ULONG* Above);
ULONG GuardLogRead(ULONG From, _Out_writes_to_(Max, return) struct _BC250_LOG_LINE* Lines, ULONG Max,
                   _Out_ ULONG* Next);
void GuardLogKeep(void);                                        // the ring into a file under C:\BC250\kmdlog; PASSIVE_LEVEL

// mmio.c
NTSTATUS MmioStart(_Inout_ BC250_DEVICE* Device);
void MmioStop(_Inout_ BC250_DEVICE* Device);
NTSTATUS MmioRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
NTSTATUS MmioWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);
NTSTATUS MmioGartRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
NTSTATUS MmioGartWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);
ULONG MmioGartTable(_Outptr_ const unsigned long** Table);
NTSTATUS MmioPspRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
NTSTATUS MmioPspWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);
NTSTATUS MmioGfxRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
NTSTATUS MmioGfxWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);
NTSTATUS MmioIhRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
NTSTATUS MmioIhWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);

// ih.c
struct _BC250_ESCAPE_IH;
NTSTATUS IhStart(_Inout_ BC250_DEVICE* Device);
void IhStop(_Inout_ BC250_DEVICE* Device);
void IhRemove(_Inout_ BC250_DEVICE* Device);
BOOLEAN IhInterrupt(_Inout_ BC250_DEVICE* Device);
void IhDpc(_Inout_ BC250_DEVICE* Device);
BOOLEAN IhIsActive(_In_ const BC250_DEVICE* Device);
void IhEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_IH* Data);

// sequence.c
void SequenceBegin(_Out_ BC250_SEQUENCE* Sequence, _In_ BC250_DEVICE* Device, BOOLEAN Plan,
                   _Out_writes_opt_(MaxWrites) BC250_SEQUENCE_WRITE* Writes, ULONG MaxWrites);

// vram.c
struct _BC250_ESCAPE_MEMORY;
NTSTATUS VramStart(_Inout_ BC250_DEVICE* Device);
void VramStop(_Inout_ BC250_DEVICE* Device);
void VramEscape(_In_ const BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_MEMORY* Data);
// Where in VRAM the firmware's framebuffer is, if its address is inside one of the two views we know. Needs the
// VRAM carve-out to have been identified (EnableVram); wddm.c carves its segment clear of the answer.
BOOLEAN VramFramebufferOffset(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset);

// gart.c
struct _BC250_ESCAPE_GART;
NTSTATUS GartStart(_Inout_ BC250_DEVICE* Device);
void GartStop(_Inout_ BC250_DEVICE* Device);
void GartEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_GART* Data);
struct amdgpu_device;
// With GartLock held: the shim's device (register bases, VRAM window), set up if it was not yet, and whether the
// GART sequence is enabled.
NTSTATUS GartDevice(_In_ BC250_DEVICE* Device, _Outptr_ struct amdgpu_device** Adev, _Out_ BOOLEAN* Enabled);

// psp.c
struct _BC250_ESCAPE_PSP;
NTSTATUS PspStart(_Inout_ BC250_DEVICE* Device);
void PspStop(_Inout_ BC250_DEVICE* Device);
void PspEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_PSP* Data);
BOOLEAN PspIsLoaded(_In_ const BC250_DEVICE* Device);

// gpumem.c: GPU-visible memory and doorbells for the sequences. All with GartLock held.
struct _BC250_ESCAPE_DOORBELL;
NTSTATUS GpuMemStart(_Inout_ BC250_DEVICE* Device);
void GpuMemStop(_Inout_ BC250_DEVICE* Device, BOOLEAN GpuQuiet);
void GpuMemRelease(_Inout_ BC250_DEVICE* Device, _In_ const VOID* Owner, BOOLEAN GpuQuiet);
ULONGLONG GpuMemDoorbellBase(_In_ const BC250_DEVICE* Device);
void GpuMemBeginSequence(_Inout_ BC250_DEVICE* Device, _Out_writes_opt_(MaxDoorbells) struct _BC250_ESCAPE_DOORBELL* Doorbells,
                         ULONG MaxDoorbells);
ULONG GpuMemEndSequence(_Inout_ BC250_DEVICE* Device, _Out_ ULONG* VramBytes, _Out_ ULONG* GttBytes);

// gfx.c
struct _BC250_ESCAPE_GFX;
NTSTATUS GfxStart(_Inout_ BC250_DEVICE* Device);
void GfxStop(_Inout_ BC250_DEVICE* Device);
void GfxEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_GFX* Data);
BOOLEAN GfxIsActive(_In_ const BC250_DEVICE* Device);
struct _BC250_ESCAPE_FENCE;
void GfxFenceEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_FENCE* Data);
// ADR 0008 stage C: one indirect buffer on the gfx ring. The ring side of a submission lives here so that wddm.c
// stays free of shim types, exactly as it is today.
//   GfxSubmitIb      PASSIVE_LEVEL only; takes Device->GartLock. Programs VMID Vmid's page directory root if
//                    RootPhysical differs from what that VMID was last given in this device start (Vmid 0 has no root
//                    of its own and never programs one), then submits the IB with an interrupting fence and rings the
//                    doorbell. It does NOT wait. One submission is in flight at a time: while the previous sequence
//                    number has not arrived it answers STATUS_DEVICE_BUSY and writes nothing.
//   GfxFenceArrived  DISPATCH_LEVEL: one read of the fence slot in GTT memory, no lock, no register. It also clears
//                    the in-flight mark, so it is what lets the next submission through.
//   GfxSubmitReady   whether a submission would be taken: stage 8 done, nothing failed, nothing in flight and the
//                    EnableGpuSubmit gate open. Advisory - GfxSubmitIb checks the same things under the lock.
//   GfxSubmitFail    sticky, callable at DISPATCH_LEVEL: nothing is written to the ring through GfxSubmitIb again in
//                    this device start. There is no GPU reset on this part (facts M53), so abandoning the path is the
//                    only safe answer to a submission that never completed.
NTSTATUS GfxSubmitIb(_Inout_ BC250_DEVICE* Device, ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress,
                     ULONG SizeBytes, _Out_ ULONG* Seq);
BOOLEAN GfxFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq);
BOOLEAN GfxSubmitReady(_In_ const BC250_DEVICE* Device);
void GfxSubmitFail(_Inout_ BC250_DEVICE* Device);

// pnp.c
DXGKDDI_ADD_DEVICE Bc250AddDevice;
DXGKDDI_START_DEVICE Bc250StartDevice;
DXGKDDI_STOP_DEVICE Bc250StopDevice;
DXGKDDI_REMOVE_DEVICE Bc250RemoveDevice;
DXGKDDI_RESET_DEVICE Bc250ResetDevice;
DXGKDDI_DISPATCH_IO_REQUEST Bc250DispatchIoRequest;
DXGKDDI_INTERRUPT_ROUTINE Bc250InterruptRoutine;
DXGKDDI_DPC_ROUTINE Bc250DpcRoutine;
DXGKDDI_QUERY_CHILD_RELATIONS Bc250QueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS Bc250QueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR Bc250QueryDeviceDescriptor;
DXGKDDI_SET_POWER_STATE Bc250SetPowerState;
DXGKDDI_UNLOAD Bc250Unload;
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP Bc250StopDeviceAndReleasePostDisplayOwnership;

// display.c
DXGKDDI_QUERYADAPTERINFO Bc250QueryAdapterInfo;
DXGKDDI_SETPOINTERPOSITION Bc250SetPointerPosition;
DXGKDDI_SETPOINTERSHAPE Bc250SetPointerShape;
DXGKDDI_ESCAPE Bc250Escape;
DXGKDDI_ISSUPPORTEDVIDPN Bc250IsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN Bc250RecommendFunctionalVidPn;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY Bc250EnumVidPnCofuncModality;
DXGKDDI_SETVIDPNSOURCEVISIBILITY Bc250SetVidPnSourceVisibility;
DXGKDDI_COMMITVIDPN Bc250CommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH Bc250UpdateActiveVidPnPresentPath;
DXGKDDI_RECOMMENDMONITORMODES Bc250RecommendMonitorModes;
DXGKDDI_QUERYVIDPNHWCAPABILITY Bc250QueryVidPnHWCapability;
DXGKDDI_PRESENTDISPLAYONLY Bc250PresentDisplayOnly;
DXGKDDI_SYSTEM_DISPLAY_ENABLE Bc250SystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE Bc250SystemDisplayWrite;

NTSTATUS DisplayMapFramebuffer(_Inout_ BC250_DEVICE* Device);
void DisplayUnmapFramebuffer(_Inout_ BC250_DEVICE* Device);

// wddm.c (M7 stage A, ADR 0008). Everything here is inert while the EnableFullWddm gate is closed.
BOOLEAN WddmGateOpen(void);                     // EnableFullWddm, read once in DriverEntry
void WddmBuildTable(_Out_ DRIVER_INITIALIZATION_DATA* Data);
void WddmStart(_Inout_ BC250_DEVICE* Device);       // never fails the start, like every other bring-up file
void WddmSourceVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible);   // display.c's SetVidPnSourceVisibility
void WddmStop(_Inout_ BC250_DEVICE* Device);
void WddmDpc(_Inout_ BC250_DEVICE* Device);
void WddmGpuFence(_Inout_ BC250_DEVICE* Device);    // stage C: has the packet in flight finished? <= DISPATCH_LEVEL
// vidmm.c: VidMm's page tables (ADR 0008 stage B). EnableGpuVa 0 = plan and log, 1 = write the entries.
void VidMmStart(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId);
void VidMmStop(void);
void VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update);
void VidMmSetRootPageTable(_In_ const DXGKARG_SETROOTPAGETABLE* Root);
BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical);
BOOLEAN VidMmTranslate(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System);
void VidMmSummary(void);
void WddmSummary(_In_ BC250_DEVICE* Device);        // writes the DDI counter tables into the log ring; does nothing
                                                    // when the gate is closed, so the escape can call it either way
