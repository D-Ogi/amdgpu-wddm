// bc250kmd: WDDM miniport for the ASRock BC-250 (AMD Cyan Skillfish, PCI 1002:13FE).
// Milestone M3 (ADR 0006): display-only, keeps the firmware's framebuffer. MMIO is a separate, gated part
// (ADR 0007, mmio.c) that the display path never uses.
//
// Layout: entry.c (DriverEntry, DDI table), pnp.c (device life cycle, children, power),
// display.c (VidPN, present, pointer, system display), guard.c (boot-loop guard, breadcrumbs, log),
// mmio.c (gated register access for the bring-up experiments).
#pragma once

#include <ntifs.h>        // superset of ntddk.h; the token checks of the escape need it
#include <windef.h>
#include <dispmprt.h>

#define BC250_TAG 'dK52'
#define BC250_CHILD_UID 0x250001        // the one DisplayPort output, as far as this driver is concerned
#define BC250_MAX_UNCONFIRMED_STARTS 2  // guard.c: refuse to start after this many starts nobody confirmed

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

    // The VRAM carve-out, all zero unless the EnableVram gate was open at start (vram.c).
    BOOLEAN VramEnabled;
    BOOLEAN VramWriteEnabled;
    PHYSICAL_ADDRESS VramPhysical;      // system physical address of VRAM byte 0
    ULONGLONG VramLength;
    ULONGLONG VramMcBase;               // GPU physical (MC) address of VRAM byte 0
    PHYSICAL_ADDRESS Bar0Physical;
    ULONGLONG Bar0Length;
} BC250_DEVICE;

// guard.c
NTSTATUS GuardInit(_In_ PUNICODE_STRING RegistryPath);
void GuardCleanup(void);
void GuardStage(BC250_STAGE Stage);
BC250_STAGE GuardLastStage(void);
NTSTATUS GuardCheckAndCountStart(void);         // STATUS_SUCCESS, or a failure when the start budget is used up
void GuardLog(_In_z_ const char* Format, ...);
ULONG GuardReadSetting(_In_z_ PCWSTR Name, ULONG Default);     // REG_DWORD under Parameters, PASSIVE_LEVEL

// mmio.c
NTSTATUS MmioStart(_Inout_ BC250_DEVICE* Device);
void MmioStop(_Inout_ BC250_DEVICE* Device);
NTSTATUS MmioRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
NTSTATUS MmioWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value);

// vram.c
struct _BC250_ESCAPE_MEMORY;
NTSTATUS VramStart(_Inout_ BC250_DEVICE* Device);
void VramStop(_Inout_ BC250_DEVICE* Device);
void VramEscape(_In_ const BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_MEMORY* Data);

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
