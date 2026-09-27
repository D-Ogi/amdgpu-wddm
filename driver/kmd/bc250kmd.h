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
#include "paging_window.h"
#include "paging_graph_batch.h"
#include "paging_capture.h"
#include "smu.h"
#include "start_health.h"

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

#define BC250_VISIBILITY_HISTORY_COUNT 16u
typedef struct _BC250_VISIBILITY_EVENT {
    ULONG Call, Source, Requested;
    NTSTATUS Status;
    ULONG SourceVisible, Blanked;
    LONGLONG BeginQpc, EndQpc;
} BC250_VISIBILITY_EVENT;

typedef struct _BC250_DEVICE {
    BC250_START_HEALTH_STATE StartHealth;
    volatile LONG RetainedPowerPhase; // 0 active, 1 suspending, 2 suspended, 3 restoring, 4 failed
    DEVICE_POWER_STATE RetainedDownState;
    POWER_ACTION RetainedDownAction;
    PDEVICE_OBJECT PhysicalDeviceObject;
    DXGKRNL_INTERFACE Dxgk;
    DXGK_START_INFO StartInfo;
    DXGK_DEVICE_INFO DeviceInfo;

    BOOLEAN Started;
    BOOLEAN SourceVisible;
    BOOLEAN ModeActive;         // a commit has validated that the source surface is exactly the firmware's mode
    BOOLEAN PresentSeen;
    BOOLEAN CommitSeen;
    // Retained normal-DDI diagnostics, independent of the wrapping guard log.
    // Level Two visibility/commit/summary exclusion protects each last-request tuple.
    volatile LONG VisibilityCalls;
    ULONG VisibilityLastSource;
    BOOLEAN VisibilityLastRequested;
    NTSTATUS VisibilityLastStatus;
    ULONG VisibilityTrueCalls, VisibilityFalseCalls, VisibilityFailures;
    NTSTATUS VisibilityFirstTrueStatus, VisibilityLastTrueStatus;
    BC250_VISIBILITY_EVENT VisibilityHistory[BC250_VISIBILITY_HISTORY_COUNT];
    volatile LONG CommitPowerCalls;
    BOOLEAN CommitLastPowerTransition, CommitLastPoweredOff;

    // The mode the firmware left. M3 offers exactly this one.
    DXGK_DISPLAY_INFORMATION Post;
    D3DKMDT_VIDEO_SIGNAL_INFO InheritedSignal;
    BOOLEAN InheritedSignalValid;     // immutable from successful start until stop
    PVOID Framebuffer;                  // mapping of Post.PhysicAddress
    SIZE_T FramebufferLength;
    BOOLEAN SystemDisplayReady;        // bugcheck CPU writes only after verified scanout restore
    BOOLEAN PostDisplayStopAttempted;  // WddmStop restores before freeing scanout objects
    NTSTATUS PostDisplayStopStatus;    // separate from the ordinary StopDevice completion result
    ULONG FramebufferCacheProtect;     // actual successful POST mapping cache attribute
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;

    // BAR5, NULL unless the EnableMmio gate was open at start (mmio.c).
    BC250_SMU_OWNER Smu;               // initialized at AddDevice; online only after explicit writer handover
    volatile ULONG* Mmio;
    PHYSICAL_ADDRESS MmioPhysical;
    BOOLEAN MmioWriteEnabled;
    BOOLEAN MmioGartEnabled;
    BOOLEAN MmioPspEnabled;
    BOOLEAN MmioGfxEnabled;

    // dcn.c's gated flip (0.7.20, ADR 0011 point 3 step 2). DcnWriteEnabled is EnableMmio && EnableDcnWrite
    // (mmio.c's MmioStart). The other four are the flip's own memory: the firmware's own HUBP0 address, captured
    // once per device start on the first DcnFlip call of any kind (flip, fill or restore) before anything is
    // written; the address the most recent successful flip left HUBP0 at; and whether that address is still the
    // firmware's - which is what the stop path (pnp.c) asks before it calls DcnRestore.
    BOOLEAN DcnWriteEnabled;
    volatile LONG DcnSurfaceSequence; // coherent address/pitch snapshot for CPU mapping
    ULONG DcnFirmwarePitch;
    ULONG DcnCurrentPitch;
    BOOLEAN DcnFirmwareKnown;
    ULONGLONG DcnFirmwareAddress;
    ULONGLONG DcnCurrentAddress;
    BOOLEAN DcnDiverged;
    BOOLEAN DcnBlanked;                // this driver requested blank; restore must unblank

    // dcn.c's VidPn flip (0.7.24, ADR 0011 point 3 step 3): Device->DcnWriteEnabled && EnableVidPnFlip together
    // (mmio.c's MmioStart), i.e. EnableMmio && EnableDcnWrite && EnableVidPnFlip. Meaningful only under the full
    // table: the display-only DDI table never calls DxgkDdiSetVidPnSourceAddress or DxgkDdiControlInterrupt.
    BOOLEAN VidPnFlipEnabled;
    // The hardware vsync (OTG0's VUPDATE_NO_LOCK, facts M88): armed by wddm.c's WddmVSyncArm in place of the
    // software timer while VidPnFlipEnabled is open. DcnVsyncArmed is read lock-free by the ISR (DIRQL,
    // dcn.c's DcnVsyncInterrupt), the same acceptable race as ih.c's own Active; DcnVsyncAcked is what the ISR
    // hands the vsync DPC (WddmDcnVsync, wddm.c) once per real event it found and acknowledged.
    // Diagnostic samples, not one atomic snapshot. Times are KeQueryInterruptTime
    // (100 ns, clock-tick resolution); zero means no sample since device start.
    volatile LONG64 InterruptLastTime;
    volatile LONG64 DcnVsyncEntryTime;
    volatile LONG64 DcnVsyncAckTime;
    volatile LONG64 DcnVsyncNotifyTime; // NotifyInterrupt returned, not OS acceptance
    volatile LONG DcnVsyncNoMmio, DcnVsyncFlipDisabled, DcnVsyncUnarmed;
    volatile LONG DcnVsyncNoEvent, DcnVsyncReadFailed, DcnVsyncAckFailed;
    volatile LONG DcnVsyncLastStatus; // raw OTG_GLOBAL_SYNC_STATUS, valid after a successful read
    volatile LONG DcnVsyncArmed;
    volatile LONG DcnVsyncAcked;
    volatile LONG DcnVsyncTicks;         // every VUPDATE_NO_LOCK event the ISR acknowledged, armed or not
    volatile LONG DcnVsyncRefused;       // MmioDcnRead/MmioDcnWrite failed inside the ISR (should not happen:
                                         // BAR5 stays mapped for the whole device start; counted, not assumed impossible)
    volatile LONG DcnVsyncDeferred;      // completion observation deferred (may report the old buffer)
    volatile LONG DcnVsyncOldBufferReports; // vblanks preserved with a distinct observed scanout
    volatile LONG DcnFlipsHardware;      // SetVidPnSourceAddress flips that reached the M87 write sequence
    volatile LONG DcnLockTimeouts;      // bounded OTG update-lock acknowledgement expired
    volatile LONG DcnFlipRefused;        // SetVidPnSourceAddress translation/range or programming refused

    // 2026-09-22 (ADR 0011 consequences, facts M97/M100): the present path's own destination once the flip is
    // live - a CPU mapping of whatever DcnCurrentAddress currently names, next to that field for the same
    // reason DcnDiverged sits next to it. Owned by dcn.c's DcnScanoutMapping/DcnUnmapScanout and consulted only
    // by wddm.c's WddmPresentBlit, PASSIVE_LEVEL only - never DcnFlipSourceAddress itself, which may run above
    // DISPATCH_LEVEL and must never call MmMapIoSpaceEx/MmUnmapIoSpace (see dcn.c). NULL/0 whenever nothing is
    // mapped: gate closed, no flip yet, DcnDiverged clear, or the last mapping attempt failed.
    PVOID DcnScanoutMap;
    ULONGLONG DcnScanoutMapAddress;      // the physical address DcnScanoutMap corresponds to
    SIZE_T DcnScanoutMapLength;
    ULONGLONG DcnScanoutSeedAddress;     // firmware framebuffer copied here once; 0 until then, cleared on unmap
    volatile LONG DcnScanoutRemaps;      // DcnScanoutMapping: successful (re)maps of the flip target
    volatile LONG DcnScanoutMapFailed;   // DcnScanoutMapping: AddressAllowed or MmMapIoSpaceEx refused it

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
    KSPIN_LOCK GfxAccessLock;           // protects DPC reader admission and reference count
    KEVENT GfxAccessDrained;
    ULONG GfxAccessUsers;
    BOOLEAN GfxAccessClosed;
    EX_PUSH_LOCK GfxPagingLock;         // builder shared, GfxEscape/Stop exclusive; acquired before GartLock
    PVOID Gfx;                          // gfx.c, the same
    PVOID Ih;                           // ih.c, NULL unless the EnableIh gate was open at start
    BOOLEAN FullWddm;                   // DriverEntry found EnableFullWddm open and gave dxgkrnl the full table
    PAGING_APERTURE WddmAperture;        // immutable geometry for this device start; no owned pointer
    PVOID Wddm;                         // wddm.c, NULL unless FullWddm
    BOOLEAN MmioIhEnabled;
    BOOLEAN GfxStopPrepared;            // hardware retirement attempted in this device generation
    BOOLEAN GartStopPrepared;
    BOOLEAN GfxTlbBootstrap;            // unpublished startup owes GFX visibility before CP
    BOOLEAN GartStopQuiet;              // contexts/caches disabled, owner retained for final cleanup
    BOOLEAN GfxStopQuiet;               // stop verdict, not a runtime readiness flag
    BOOLEAN PspStopQuiet;               // PSP has released ring/TMR at stop
    BOOLEAN GpuStopUnconfirmed;         // sticky for this device object; prevents unsafe restart
    BOOLEAN IhQuiet;                    // ih.c's stop: the IH ring reads disabled (or never was enabled); for gpumem.c's stop
    volatile LONG InterruptCount;       // every call of the interrupt routine since start, ours or not
    volatile LONG LastMessageNumber;
    BOOLEAN InterruptIsMessage;         // what Windows assigned (pnp.c, from the translated resources)
    ULONG InterruptVector;
    FAST_MUTEX GartLock;                // serializes every bring-up sequence (gart.c, psp.c, gfx.c) and the stop;
                                        // initialized in AddDevice
} BC250_DEVICE;

void StartHealthInitialize(BC250_DEVICE* Device);
void StartHealthBegin(BC250_DEVICE* Device, BOOLEAN Full);
void StartHealthReady(BC250_DEVICE* Device, BOOLEAN Ready);
void StartHealthResumeReady(BC250_DEVICE* Device, BOOLEAN Mode);
void StartHealthClose(BC250_DEVICE* Device);
void StartHealthFault(BC250_DEVICE* Device);
void StartHealthRemove(BC250_DEVICE* Device);
void StartHealthEnter(BC250_DEVICE* Device);
void StartHealthLeave(BC250_DEVICE* Device);
void StartHealthDisplayLocked(BC250_DEVICE* Device, BOOLEAN Visible, BOOLEAN Mode);
void StartHealthVisibilityLocked(BC250_DEVICE* Device, BOOLEAN Visible);
void StartHealthCompleted(BC250_DEVICE* Device, ULONG Sequence);
void StartHealthRequest(BC250_DEVICE* Device, BC250_ESCAPE_START_HEALTH* Data, BOOLEAN Admin, ULONG EscapeFlags);
NTSTATUS GuardConfirmStartDurable(void);


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
    BOOLEAN TraceRlcRetirement;         // scoped passive GFX teardown diagnostics, never a quiet predicate
    BOOLEAN TraceBootstrapTlb;          // only inside the PASSIVE-safe unpublished RLC stage scope
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
NTSTATUS GuardCheckAndCountStart(BOOLEAN RequireDurable); // full table needs a durably recorded start
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
// ADR 0011 point 3: the DCN dump's own registers (dcn.c), read-only, no write side of this pair. What
// MmioDcnTable() hands back is generated by gen_regs.py (regs.generated.h: g_DcnRegisters, BC250_DCN_REG_INFO_COUNT).
typedef struct _BC250_DCN_REG_INFO {
    const char* Name;                   // the mm* name (third_party/linux-amdgpu/dcn_2_0_1_offset.h), "mm" dropped
    unsigned long Offset;               // BAR5 byte offset (tools/regcalc, ip DMU)
} BC250_DCN_REG_INFO;
NTSTATUS MmioDcnRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
ULONG MmioDcnTable(_Outptr_ const BC250_DCN_REG_INFO** Table);
// 0.7.20 (ADR 0011 point 3 step 2): the write side, g_MmioDcnWriteAllow's six HUBP0/OTG0 registers only, gated
// by Device->DcnWriteEnabled. Every call logged - the escape's own occasional writes (dcn.c's DcnFlipCore).
NTSTATUS MmioDcnWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);
// 0.7.24 (ADR 0011 point 3 step 3, review 16 section 24): the same validated write, Quiet skips the per-write
// GuardLog. For the two hot paths this step adds - DcnVsyncInterrupt's ack, once every vblank at DIRQL for as
// long as the hardware vsync stays armed, and DcnFlipWriteSequence when called from the DDI's own
// DcnFlipSourceAddress (already summarized by its own capped GuardLog) - so a live desktop does not call
// DbgPrintEx from the ISR on every frame forever.
NTSTATUS MmioDcnWriteEx(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value, BOOLEAN Quiet);

// ih.c
struct _BC250_ESCAPE_IH;
NTSTATUS IhStart(_Inout_ BC250_DEVICE* Device);
void IhStop(_Inout_ BC250_DEVICE* Device);
// PASSIVE retained power transition; same GART mappings required before resume.
NTSTATUS IhSetPowerRetained(BC250_DEVICE* Device, BOOLEAN Resume);
void IhRemove(_Inout_ BC250_DEVICE* Device);
BOOLEAN IhInterrupt(_Inout_ BC250_DEVICE* Device);
void IhDpc(_Inout_ BC250_DEVICE* Device);
BOOLEAN IhIsActive(_In_ const BC250_DEVICE* Device);
void IhEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_IH* Data);
// PASSIVE_LEVEL; caller supplies nonpaged report storage, retained through completion.
NTSTATUS IhInitializeHardware(BC250_DEVICE* Device, struct _BC250_ESCAPE_IH* Report);

// dcn.c (ADR 0011 point 3): a read-only dump, no gate of its own beyond BAR5 being mapped, no sequence, no
// Device state - so, unlike gart.c/psp.c/gfx.c/ih.c, no Start/Stop and no GartLock.
struct _BC250_ESCAPE_DCN_OBSERVE;
void DcnObserve(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_DCN_OBSERVE* Data,
    _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags);
struct _BC250_ESCAPE_DCN;
void DcnEscape(_In_ const BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_DCN* Data);

// dcn.c (BC250_ESCAPE_RUN_FBDUMP, 2026-09-22): a read-only band of the scanned-out surface's pixels, for
// bc250kmd_cli fbdump / mon.py scanout. PASSIVE_LEVEL only (MmMapIoSpaceEx); no register write; no gate beyond
// EnableMmio, the same condition DcnEscape answers to.
struct _BC250_ESCAPE_FBDUMP;
void FbdumpEscape(_In_ const BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_FBDUMP* Data);

// dcn.c (0.7.20, ADR 0011 point 3 step 2): the gated flip. PASSIVE_LEVEL only (KeStallExecutionProcessor's poll,
// MmMapIoSpaceEx for the optional fill). Physical is a system physical address (M31); ignored when Restore is
// set, when the target is instead Device->DcnFirmwareAddress. No GartLock: DMU is not on the register set any
// GART/PSP/GFX/IH sequence touches, so nothing here can race a bring-up sequence, only a second flip escape -
// which dxgkrnl already serializes through Escape->Flags.HardwareAccess like every write escape in this driver.
struct _BC250_ESCAPE_DCNFLIP;
void DcnFlipEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_DCNFLIP* Data);
// The stop path's undo (pnp.c, ADR 0011 consequences): flips back to the firmware's address when Device->DcnDiverged
// says the most recent flip left it somewhere else. A no-op, logged as one, when it does not - so pnp.c can call
// it unconditionally rather than reach into dcn.c's state.
NTSTATUS DcnStop(_Inout_ BC250_DEVICE* Device);
NTSTATUS DcnSetVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible);
NTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device); // any IRQL; no allocation/log/lock

// dcn.c (0.7.24, ADR 0011 point 3 step 3): SetVidPnSourceAddress's own path - the same M87 write sequence
// DcnFlipEscape uses (step 2), never PollFlipPending's blocking poll (this may run above DISPATCH_LEVEL, per
// d3dkmddi.h's own annotation of DXGKDDI_SETVIDPNSOURCEADDRESS: PASSIVE_LEVEL..PROFILE_LEVEL-1). CardAddress is
// what dxgkrnl's DXGKARG_SETVIDPNSOURCEADDRESS.PrimaryAddress carries (wddm.c's segment BaseAddress numbers,
// Device->VramMcBase + an offset), not the physical address DCN wants; PhysicalOut, if not NULL, gets the
// translated address for the caller's own log line. STATUS_DEVICE_NOT_READY with the gate closed,
// STATUS_ACCESS_DENIED when the address does not translate inside the carve-out or fails the escape's own 4
// KiB/range rule.
NTSTATUS DcnFlipSourceAddress(_Inout_ BC250_DEVICE* Device, ULONGLONG CardAddress, ULONG Pitch, ULONGLONG AllocationBytes, _Out_opt_ ULONGLONG* PhysicalOut);
// Non-blocking hardware observations: pending includes EARLIEST_INUSE mismatch.
// Scanout returns the actual card address, independently of request publication.
BOOLEAN DcnFlipPending(_In_ const BC250_DEVICE* Device, ULONGLONG RequestedAddress);
NTSTATUS DcnReadScanoutAddress(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* CardAddress);
NTSTATUS DcnReadScanLine(_In_ const BC250_DEVICE* Device, _Out_ BOOLEAN* InBlank, _Out_ UINT* ScanLine);
// The hardware vsync interrupt's own enable/ack (0.7.24, ADR 0011 point 3 step 3). DcnVsyncEnable is
// wddm.c's WddmVSyncArm's hardware branch: a read-modify-write of OTG0_OTG_GLOBAL_SYNC_STATUS that sets or
// clears VUPDATE_NO_LOCK_INT_EN and always acks whatever VUPDATE_NO_LOCK_EVENT_CLEAR finds pending, so neither
// turning the source on nor off can leave a stale event behind. DcnVsyncInterrupt is pnp.c's
// Bc250InterruptRoutine's own call, at DIRQL: a no-op unless Device->VidPnFlipEnabled and Device->DcnVsyncArmed
// are both true, otherwise one MmioDcnRead and, only when VUPDATE_NO_LOCK_EVENT_OCCURRED is set, one quiet
// MmioDcnWriteEx that acks it and queues the DPC. Neither takes a lock (mmio.c's MmioDcnRead/MmioDcnWriteEx
// take none), which is what makes the second one legal at DIRQL - Quiet=TRUE is what keeps it fast (review 16
// section 24: no DbgPrintEx from here on every vblank).
NTSTATUS DcnVsyncEnable(_Inout_ BC250_DEVICE* Device, BOOLEAN On);
BOOLEAN DcnVsyncInterrupt(_Inout_ BC250_DEVICE* Device);

// 2026-09-22 (ADR 0011 consequences, facts M97/M100): the present path's own destination once the flip has
// moved the scanout away from the firmware's framebuffer. PASSIVE_LEVEL only (MmMapIoSpaceEx/MmUnmapIoSpace);
// the only caller is wddm.c's WddmPresentBlit, which already requires PASSIVE_LEVEL for its own source
// mapping. DcnScanoutMapping remaps only when Device->DcnCurrentAddress changed since the last call - once a
// flip, not once a present (M97: one flip served 60 presents) - and re-validates the address with the same
// AddressAllowed/DcnAddressFits rule DcnFlipSourceAddress itself refused it against, never trusting that a
// past check still holds. FALSE (*Mapping left NULL) whenever the flip is not live (Device->DcnDiverged
// clear) or the mapping could not be made, either way leaving the POST framebuffer as the caller's own
// fallback - this function never decides that, it only reports what it could map.
// DcnUnmapScanout tears the mapping down; called from WddmStop and, idempotently, from DcnStop
// (docs/design/vidpn-flip.md section 8's stop order has WddmStop run first, so DcnStop's own call finds it
// already NULL in the ordinary case).
BOOLEAN DcnScanoutMapping(_Inout_ BC250_DEVICE* Device, _Out_ PVOID* Mapping, _Out_ SIZE_T* Length, _Out_ ULONG* Pitch);
void DcnUnmapScanout(_Inout_ BC250_DEVICE* Device);

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
PVOID VramMapCpuRange(_In_ const BC250_DEVICE* Device, PHYSICAL_ADDRESS Physical, SIZE_T Length, ULONG Access);
ULONG VramMappingProtection(_In_ const BC250_DEVICE* Device, ULONGLONG Physical, SIZE_T Length, ULONG Access);
BOOLEAN VramFramebufferOffset(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset);

// gart.c
struct _BC250_ESCAPE_GART;
NTSTATUS GartStart(_Inout_ BC250_DEVICE* Device);
void GartPrepareStop(_Inout_ BC250_DEVICE* Device);
void GartStop(_Inout_ BC250_DEVICE* Device);
void GartEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_GART* Data);
// PASSIVE_LEVEL; caller supplies nonpaged report storage, retained through completion.
// Retained-power ownership predicates require GartLock; they do not touch MMIO.
BOOLEAN GfxPowerIsSuspended(const BC250_DEVICE* Device);
NTSTATUS GpuSetPowerRetained(BC250_DEVICE* Device, DEVICE_POWER_STATE State, POWER_ACTION Action);
NTSTATUS GfxSetPowerRetained(BC250_DEVICE* Device, BOOLEAN Resume);
BOOLEAN PspPowerIsSuspended(const BC250_DEVICE* Device);
NTSTATUS GartSetPowerRetained(BC250_DEVICE* Device, BOOLEAN Resume);
NTSTATUS PspSetPowerRetained(BC250_DEVICE* Device, BOOLEAN Resume, struct _BC250_ESCAPE_PSP* Report);
NTSTATUS GartInitializeHardware(BC250_DEVICE* Device, struct _BC250_ESCAPE_GART* Report);
struct amdgpu_device;
// With GartLock held: the shim's device (register bases, VRAM window), set up if it was not yet, and whether the
// GART sequence is enabled.
NTSTATUS GartDevice(_In_ BC250_DEVICE* Device, _Outptr_ struct amdgpu_device** Adev, _Out_ BOOLEAN* Enabled);
// PASSIVE_LEVEL: captures geometry under GartLock, no hardware enable or PTE write.
NTSTATUS GartCaptureAperture(BC250_DEVICE* Device, PAGING_APERTURE* Aperture);


// psp.c
struct _BC250_ESCAPE_PSP;
NTSTATUS PspStart(_Inout_ BC250_DEVICE* Device);
void PspStop(_Inout_ BC250_DEVICE* Device);
void PspEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_PSP* Data);
// PASSIVE_LEVEL; caller supplies nonpaged report storage, retained through completion.
NTSTATUS PspInitializeHardware(BC250_DEVICE* Device, struct _BC250_ESCAPE_PSP* Report);
struct _BC250_PSP_FIRMWARE;
// PASSIVE_LEVEL; prepare has no hardware writes. Caller owns/release on every path.
NTSTATUS PspPrepareFirmware(BC250_DEVICE* Device, struct _BC250_ESCAPE_PSP* Report,
                           struct _BC250_PSP_FIRMWARE** Firmware);
NTSTATUS PspInitializePrepared(BC250_DEVICE* Device, const struct _BC250_PSP_FIRMWARE* Firmware,
                              struct _BC250_ESCAPE_PSP* Report);
void PspReleaseFirmware(struct _BC250_PSP_FIRMWARE* Firmware);

BOOLEAN PspIsLoaded(_In_ const BC250_DEVICE* Device);

// gpumem.c: GPU-visible memory and doorbells for the sequences. All with GartLock held.
struct _BC250_ESCAPE_DOORBELL;
NTSTATUS GpuMemStart(_Inout_ BC250_DEVICE* Device);
void GpuMemStop(_Inout_ BC250_DEVICE* Device, BOOLEAN GpuQuiet);
void GpuMemRelease(_Inout_ BC250_DEVICE* Device, _In_ const VOID* Owner, BOOLEAN GpuQuiet);
// Retire current sequence GTT mappings after CP/SDMA drain, retaining backing.
int GpuMemRetireGttMappings(struct amdgpu_device* Adev);
// All consumers halted, GartLock held; rebuild private prefix only, no TLB flush.
int GpuMemRebuildRetainedGtt(struct amdgpu_device* adev);
int GpuMemCompleteGfxBootstrap(struct amdgpu_device* Adev);
NTSTATUS GfxBeginTranslationBootstrap(BC250_DEVICE* Device);
ULONGLONG GpuMemDoorbellBase(_In_ const BC250_DEVICE* Device);
void GpuMemBeginSequence(_Inout_ BC250_DEVICE* Device, _Out_writes_opt_(MaxDoorbells) struct _BC250_ESCAPE_DOORBELL* Doorbells,
                         ULONG MaxDoorbells);
ULONG GpuMemEndSequence(_Inout_ BC250_DEVICE* Device, _Out_ ULONG* VramBytes, _Out_ ULONG* GttBytes);

// gfx.c
struct _BC250_ESCAPE_GFX;
NTSTATUS GfxStart(_Inout_ BC250_DEVICE* Device);
void GfxTraceRlcState(_In_ const BC250_DEVICE* Device, _In_ const char* Phase);
NTSTATUS GfxPreparePspReload(_Inout_ BC250_DEVICE* Device);
void GfxPrepareStop(_Inout_ BC250_DEVICE* Device);
void GfxStop(_Inout_ BC250_DEVICE* Device);
void GfxEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_GFX* Data);
// PASSIVE_LEVEL; caller supplies nonpaged report storage, retained through completion.
NTSTATUS GfxInitializeHardware(BC250_DEVICE* Device, struct _BC250_ESCAPE_GFX* Report);
BOOLEAN GfxIsActive(_In_ const BC250_DEVICE* Device);
// Unpublished, exclusively owned startup lifecycle; PASSIVE_LEVEL only.
BOOLEAN GfxStartupResources(BC250_DEVICE* Device, BOOLEAN Initialized);
struct _BC250_ESCAPE_FENCE;
void GfxFenceEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_FENCE* Data);
// ADR 0013: the SDMA copy/fill positive control, gated the same way the SDMA ring test is (EnableGfx and stage 7)
// plus EnableVramWrite. PASSIVE_LEVEL only (KeStallExecutionProcessor's poll, the CPU seed and read-back).
struct _BC250_ESCAPE_SDMACOPY;
void SdmaCopyEscape(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_SDMACOPY* Data);
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
//   GfxSubmitBusy    the same gates, but the one in-flight IB has not arrived. Advisory, same as Ready.
//   GfxSubmitFail    sticky, callable at DISPATCH_LEVEL: nothing is written to the ring through GfxSubmitIb again in
//                    this device start. There is no GPU reset on this part (facts M53), so abandoning the path is the
//                    only safe answer to a submission that never completed.
NTSTATUS GfxSubmitIb(_Inout_ BC250_DEVICE* Device, ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress,
                     ULONG SizeBytes, _Out_ ULONG* Seq);
BOOLEAN GfxFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq);
BOOLEAN GfxSubmitReady(_In_ const BC250_DEVICE* Device);
BOOLEAN GfxSubmitBusy(_In_ const BC250_DEVICE* Device);
void GfxSubmitFail(_Inout_ BC250_DEVICE* Device);

// ADR 0008 stage D (docs/design/paging-node.md): node 1, DXGK_ENGINE_TYPE_COPY on SDMA0, the paging node. A
// second, parallel channel to the four above, not a generalization of them: SubmitCommand (the paging buffer's
// DDI) is _IRQL_requires_(DISPATCH_LEVEL), exactly, so this path may never take Device->GartLock and never
// touches the shim's register-sequence machinery. It writes straight into the live SDMA0 ring (adev captured
// once, under GartLock, the same way ih.c's DpcAdev is) under its own spinlock (BC250_GFX::Sdma0RingLock),
// which every other writer of that ring (the ring test, GfxFenceEscape's SDMA arm, SdmaCopyEscape) also takes,
// narrowly, around their own ring push. VidMmTranslate returns a system physical address; paging_mc.c turns
// that into the MC address the packet wants (the number M95 ran) before anything is emitted. No VMID is
// pointed at anything for this path (design note section 2).
// GfxPagingBuild emits page-wise AMD packets into a caller-owned private payload.
// It returns both command DWORD count and cumulative data-byte progress. A partial
// prefix may accompany STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER.
// Reasons a TRANSFER_VIRTUAL/FILL_VIRTUAL is not built, for the caller's own counters:
typedef enum _BC250_WDDM_PAGING_UNSUPPORTED
{
    BC250PagingSupported = 0,           // built; not a reason
    BC250PagingNotReady,                // the gate is open but RUN has not yet reached stage 8: no live ring
    BC250PagingNoRoot,                  // hSystemContext named no context with a recorded root
    BC250PagingNoTranslation,           // VidMmTranslate refused (not ready, or the address does not resolve)
    BC250PagingSystemMemory,            // resolved to system memory: no MC mapping for a dxgkrnl-owned page (note section 2)
    BC250PagingNotContiguous,           // historical counter retained; page-wise builder now splits these ranges
} BC250_WDDM_PAGING_UNSUPPORTED;
// Mdl != NULL selects OS PFNs; otherwise Address is already an MC address.
// Length bounds bytes from this endpoint's start; FirstPage applies only to MDLs.
typedef struct _BC250_PAGING_ENDPOINT {
    PMDL Mdl;
    ULONGLONG Address,Length;
    ULONG FirstPage;
    BOOLEAN Aperture; // Address is permanent aperture MC; resolve through planned physical pages.
} BC250_PAGING_ENDPOINT;
typedef struct _BC250_PAGING_COPY_SLICE {
    ULONGLONG SourcePhysical,DestinationPhysical;
    ULONG Bytes;
    BOOLEAN SourceSystem,DestinationSystem;
} BC250_PAGING_COPY_SLICE;
NTSTATUS GfxPagingBuildCopyPageEx(BC250_DEVICE* Device, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice);
NTSTATUS GfxPagingBuildVirtualCopyPage(BC250_DEVICE* Device, ULONGLONG Root,
    ULONGLONG SourceVa, ULONGLONG DestinationVa, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice);
BOOLEAN VidMmResolveAperture(ULONGLONG Mc, ULONG Bytes, ULONGLONG* Physical);
BOOLEAN VidMmApertureRangeValid(ULONGLONG Mc, ULONGLONG Bytes);
NTSTATUS VidMmCommitPagingAperture(const DXGKARG_BUILDPAGINGBUFFER* Build, ULONG Start, ULONG Next);
NTSTATUS VidMmCommitPagingTransfer(const BC250_PAGING_COPY_SLICE* Slice);
NTSTATUS VidMmCommitPagingGraph(const PAGING_GRAPH_BATCH* Graph);
NTSTATUS GfxPagingBuildCopyPage(BC250_DEVICE* Device, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice);
NTSTATUS GfxPagingBuildPageGraph(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume);
// STATUS_NOT_SUPPORTED classifies local/mixed endpoints before packet emission;
// those need the table-shadow-aware virtual path. Other failures are not fallback.
NTSTATUS GfxPagingBuildVirtualPageGraph(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume);
SIZE_T GfxPagingCaptureStorageSize(ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes);
// Does not allocate/free. Storage remains caller-owned even after failure;
// caller must not reuse it until the capture is detached and its builders drain.
NTSTATUS GfxPagingCaptureVirtualGraphInPlace(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PVOID Storage,SIZE_T StorageBytes,
    PAGING_GRAPH_CAPTURE** Capture);
NTSTATUS GfxPagingCaptureVirtualGraph(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PAGING_GRAPH_CAPTURE** Capture);
BOOLEAN GfxPagingCapturedLinearSlice(const PAGING_GRAPH_CAPTURE* Capture,ULONGLONG Progress,
    BC250_PAGING_COPY_SLICE* Slice,ULONGLONG* NextProgress);
NTSTATUS GfxPagingEmitCapturedLinear(BC250_DEVICE* Device,const PAGING_GRAPH_CAPTURE* Capture,
    PVOID Buffer,ULONG Offset,ULONG Free,ULONG* Written,BC250_PAGING_COPY_SLICE* Slice,ULONGLONG* NextProgress);
// Plan one band from immutable captured identities. Returns the batch selector's
// Done/More/NeedCycle/Invalid, without advancing the caller-owned cursor.
int GfxPagingPlanCapturedGraph(PAGING_GRAPH_CAPTURE* Capture,unsigned Band,unsigned Action,
    unsigned MaxMoves,PAGING_GRAPH_BATCH* Batch,unsigned* NextBand,unsigned* NextAction);
NTSTATUS GfxPagingEmitCapturedGraph(BC250_DEVICE* Device,PAGING_GRAPH_CAPTURE* Capture,
    unsigned Band,unsigned Action,PVOID Buffer,ULONG Offset,ULONG Free,ULONG* Written,
    PAGING_GRAPH_BATCH* Batch,unsigned* NextBand,unsigned* NextAction);
NTSTATUS GfxPagingCheckDisjoint(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,BOOLEAN* Disjoint);
BOOLEAN GfxPagingEndpointValid(const BC250_DEVICE* Device,
    const BC250_PAGING_ENDPOINT* Endpoint, ULONGLONG Bytes);
BOOLEAN WddmPreparePhysicalTransfer(const BC250_DEVICE* Device,
    const DXGKARG_BUILDPAGINGBUFFER* Build, BC250_PAGING_ENDPOINT* Source,
    BC250_PAGING_ENDPOINT* Destination, ULONGLONG* Progress);
typedef struct _BC250_PAGING_APERTURE_OP {
    PMDL Mdl;
    ULONGLONG FirstPage,PageCount,DummyPhysical;
    ULONG MdlOffset;
    BOOLEAN Unmap,CacheCoherent;
} BC250_PAGING_APERTURE_OP;
NTSTATUS GfxPagingBuildAperture(BC250_DEVICE* Device, const BC250_PAGING_APERTURE_OP* Operation,
    ULONG StartPage, PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, ULONG* NextPage);
struct PAGING_NATIVE_RESULT;
NTSTATUS GfxPagingBuildNative(BC250_DEVICE* Device, BOOLEAN Fill, ULONGLONG Source,
    ULONGLONG Destination, ULONGLONG Total, ULONG Pattern, PVOID Buffer,
    ULONGLONG DmaBase, ULONG Offset, ULONG Free, ULONG Token, struct PAGING_NATIVE_RESULT* Built);
NTSTATUS GfxPagingBuildFillPage(BC250_DEVICE* Device, ULONGLONG Root, ULONGLONG Va,
    ULONG Bytes, ULONG Pattern, PVOID Buffer, ULONG Offset, ULONG Free,
    ULONG* Written, ULONGLONG* Physical, BOOLEAN* System);
NTSTATUS GfxPagingBuildPhysical(_Inout_ BC250_DEVICE* Device,
    _In_opt_ const BC250_PAGING_ENDPOINT* Source, _In_ const BC250_PAGING_ENDPOINT* Destination,
    BOOLEAN Fill, ULONGLONG Bytes, ULONG Pattern, _Inout_ PVOID DmaBuffer,
    ULONG DmaBufferOffset, ULONG DmaBufferFree, ULONGLONG StartByte,
    _Out_ ULONG* DwordsWritten, _Out_ ULONGLONG* NextByte);
BOOLEAN GfxPagingMdlAddress(_In_ PMDL Mdl, ULONG FirstPage, ULONGLONG ByteOffset, ULONG Bytes, _Out_ ULONGLONG* Address);
NTSTATUS GfxPagingBuild(_Inout_ BC250_DEVICE* Device, ULONGLONG RootPhysical, BOOLEAN Fill, ULONGLONG SrcVa,
                        ULONGLONG DstVa, ULONGLONG Bytes, ULONG FillPattern, _Inout_ PVOID DmaBuffer,
                        ULONG DmaBufferOffset, ULONG DmaBufferFree, ULONG StartByte,
                        _Out_ ULONG* DwordsWritten, _Out_ ULONG* NextByte,
                        _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported);
NTSTATUS GfxPagingBuildCopyRange(_Inout_ BC250_DEVICE* Device, ULONGLONG Root,
    _In_ const DXGK_BUILDPAGINGBUFFER_COPY_RANGE* Range, _Inout_ PVOID DmaBuffer,
    ULONG DmaBufferOffset, ULONG DmaBufferFree, _Out_ ULONG* DwordsWritten,
    _Out_ ULONGLONG* SourcePhysical, _Out_ ULONGLONG* DestinationPhysical,
    _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported);
NTSTATUS GfxPagingBuildUpdate(_Inout_ BC250_DEVICE* Device,
                             _In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                             _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                             ULONG StartEntry, _Out_ ULONG* DwordsWritten, _Out_ ULONG* NextEntry,
                             _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported);
NTSTATUS GfxPagingBuildFlush(_Inout_ BC250_DEVICE* Device, ULONG Vmid,
                            _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                            _Out_ ULONG* DwordsWritten,
                            _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported);
// GfxSubmitPaging copies an exact validated range from OS-owned private records
// into the ring at DISPATCH_LEVEL. It never retains those CPU pointers.
NTSTATUS GfxSubmitPaging(_Inout_ BC250_DEVICE* Device, const void* PrivateData, ULONG PrivateBytes,
                          ULONGLONG Start, ULONG ByteCount, BOOLEAN VirtualAddress, _Out_ ULONG* Seq);
//   GfxPagingFenceArrived, GfxPagingSubmitReady, GfxPagingSubmitFail: node 1's own answers to GfxFenceArrived/
//   GfxSubmitReady/GfxSubmitFail above, same contracts, independent state (node 0 failing does not fail node 1
//   and vice versa - the two nodes fail on their own hardware).
BOOLEAN GfxPagingFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq);
BOOLEAN GfxPagingSubmitReady(_In_ const BC250_DEVICE* Device);
void GfxPagingSubmitFail(_Inout_ BC250_DEVICE* Device);
// Whether the paging node is enabled at all this device start (EnablePagingNode, read once at GfxStart): the
// gate wddm.c's caps and CreateContext consult to decide whether node 1 exists. FALSE whenever Device->Gfx is
// NULL (EnableGfx closed), same as every other gfx.c answer.
BOOLEAN GfxPagingNodeGate(_In_ const BC250_DEVICE* Device);
// gpumem.c: one doorbell write reached directly from Device, with no amdgpu_device/backend involved - what
// GfxSubmitPaging needs at DISPATCH_LEVEL, where bc250_shim_wdoorbell64's adev->backend dependency (gpumem.c,
// MemOf()) cannot be used (docs/design/paging-node.md section 4). Same store bc250_shim_wdoorbell64 makes,
// same bounds; DoorbellCount is not touched, this path is not one of the sequences' own doorbell budgets.
void GpuMemDoorbellWrite(_In_ const BC250_DEVICE* Device, ULONG Index, ULONGLONG Value);

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

NTSTATUS DisplayPrepareInheritedTiming(_Inout_ BC250_DEVICE* Device);
NTSTATUS DisplayMapFramebuffer(_Inout_ BC250_DEVICE* Device);
void DisplayLogFramebufferSample(_In_ BC250_DEVICE* Device);
void DisplayUnmapFramebuffer(_Inout_ BC250_DEVICE* Device);

// wddm.c (M7 stage A, ADR 0008). Everything here is inert while the EnableFullWddm gate is closed.
BOOLEAN WddmFullTableSelected(void);            // pure getter, never consumes a gate
BOOLEAN WddmGateOpen(void);                     // EnableFullWddm, read once in DriverEntry
void WddmBuildTable(_Out_ DRIVER_INITIALIZATION_DATA* Data);
NTSTATUS WddmStart(_Inout_ BC250_DEVICE* Device);   // required resources fail before paging DDIs begin
void WddmSourceVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible);   // display.c's SetVidPnSourceVisibility
void WddmStop(_Inout_ BC250_DEVICE* Device);
// PASSIVE/Level Three, software-owner retention only. Not wired to power DDIs
// until the separate hardware suspend/restore coordinator is implemented.
NTSTATUS WddmSuspendRetained(_Inout_ BC250_DEVICE* Device);
NTSTATUS WddmResumeRetained(_Inout_ BC250_DEVICE* Device);
void WddmDpc(_Inout_ BC250_DEVICE* Device);
// ADR 0011 point 3 step 3 (0.7.24): the hardware vsync's own DPC-side report - CRTC_VSYNC once per DPC run that
// found at least one VUPDATE_NO_LOCK event acknowledged (Device->DcnVsyncAcked).
// A pending flip reports the distinct scanned buffer when the generation is stable;
// ambiguous observations remain deferred rather than retiring an unlatched flip. A no-op with Device->VidPnFlipEnabled closed. See pnp.c's
// Bc250DpcRoutine and docs/design/vidpn-flip.md.
void WddmDcnVsync(_Inout_ BC250_DEVICE* Device);
void WddmGpuFence(_Inout_ BC250_DEVICE* Device);    // stage C: has the packet in flight finished? <= DISPATCH_LEVEL
// ADR 0008 stage D: node 1's own twin of WddmGpuFence above, same call sites, same IRQL bound - see pnp.c's
// Bc250DpcRoutine and docs/design/paging-node.md section 5.
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device);
// vidmm.c: VidMm's page tables (ADR 0008 stage B). EnableGpuVa 0 = plan and log, 1 = write the entries.
NTSTATUS VidMmStartLayout(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId,
                          ULONGLONG TableOffset, ULONGLONG TableLength, ULONG TableSegmentId);
NTSTATUS VidMmStart(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId);
void VidMmStop(void);
BOOLEAN VidMmEncodePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                            ULONG Start, ULONG Count, _Out_ ULONGLONG* Physical,
                            _Out_writes_(Count) ULONGLONG* Entries);
NTSTATUS VidMmCommitPagingFill(ULONGLONG Physical, ULONGLONG Bytes, ULONG Pattern);
NTSTATUS VidMmCommitPagingCopy(ULONGLONG Source, ULONGLONG Destination, ULONG Count);
NTSTATUS VidMmCommitPagingUpdate(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update, ULONG Start, ULONG Count);
BOOLEAN VidMmTranslatePagingAccess(ULONGLONG RootPhysical, ULONGLONG Va, BOOLEAN Write, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System);
BOOLEAN VidMmTranslatePaging(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System);
NTSTATUS VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update);
void VidMmSetRootPageTable(_In_ const DXGKARG_SETROOTPAGETABLE* Root);
BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical);
BOOLEAN VidMmTranslate(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System);
// How many dwords VidMmProbeIb copies, starting at Va rather than at the base of the page.
// 240 is the whole unclamped fill_g1 IB (960 bytes) and stays inside one 4K page when Va
// is page-aligned, which every UMD IB so far has been.
#define BC250_IB_PROBE_DWORDS 240u
// The leaf entry VidMmTranslate decoded, plus up to BC250_IB_PROBE_DWORDS dwords at Va.
// Dwords stay 0 when the page is not system memory, the address is not one this driver may map,
// or the dword would cross into the next page. PASSIVE_LEVEL. The walk is VidMmTranslate's.
BOOLEAN VidMmProbeIb(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Leaf, _Out_ ULONGLONG* Physical,
                     _Out_ BOOLEAN* System, _Out_writes_(BC250_IB_PROBE_DWORDS) ULONG* Dwords);
void VidMmSummary(void);
void WddmSummary(_In_ BC250_DEVICE* Device);        // writes the DDI counter tables into the log ring; does nothing
                                                    // when the gate is closed, so the escape can call it either way
void WddmCounters(_In_ const BC250_DEVICE* Device, _Out_ LONG* Blits, _Out_ LONG* Flips);    // 0/0 when closed

BOOLEAN VidMmPagingRootTracked(ULONGLONG Root);
NTSTATUS GfxPagingBuildVirtualPtes(BC250_DEVICE* Device, ULONGLONG Source,
    ULONGLONG Destination, ULONG Entries, PVOID Buffer, ULONGLONG DmaBase,
    ULONG Offset, ULONG Free, struct PAGING_NATIVE_RESULT* Built);
