// bc250kmd_cli - the lab's user-mode end of the M3 miniport (ADR 0006). It answers one open question of that
// ADR ("does dxgkrnl route D3DKMTEscape to a display-only driver?") and reads the driver's breadcrumbs when
// there is no kernel debugger to ask.
//
//   bc250kmd_cli info [hardware-id]   D3DKMTEscape(BC250_ESCAPE_GET_INFO) to the adapter with that PnP
//                                     hardware id (default PCI\VEN_1002&DEV_13FE, matched case-insensitively
//                                     as a prefix); prints the reply, or exactly which call failed and its NTSTATUS
//   bc250kmd_cli list                 every display adapter dxgkrnl knows, with its hardware id and LUID
//   bc250kmd_cli stages               LastStage / StageHistory / UnconfirmedStarts from the registry, with names
//   bc250kmd_cli confirm              UnconfirmedStarts = 0 (needs an elevated prompt)
//
// The escape is expected to fail today: the device runs Microsoft's Basic Display driver, which has no such
// private escape. That failure is a measurement too, so every step prints its own NTSTATUS instead of one
// summary "it did not work".
//
// Choosing the adapter. D3DKMTOpenAdapterFromGdiDisplayName ("\\.\DISPLAY1") only reaches an adapter that owns
// a GDI display, and it names a display, not a device: with two adapters, or while ours is installed but not
// driving the screen, it can silently open the wrong one. D3DKMTEnumAdapters2 gives handles but no way back to
// the PnP device. So we go the other way round: SetupAPI enumerates device interfaces, we pick the device whose
// SPDRP_HARDWAREID matches, and hand its interface path to D3DKMTOpenAdapterFromDeviceName. That is a direct
// PnP-id-to-adapter mapping and does not care whether the adapter is driving a screen.
//
// The interface class has to be GUID_DISPLAY_DEVICE_ARRIVAL, the one dxgkrnl registers for every graphics
// device including display-only ones (ntddvdeo.h). Measured on the development PC, 2026-09-21: the paths of
// GUID_DEVINTERFACE_DISPLAY_ADAPTER, which is the obvious-looking name, are refused by
// D3DKMTOpenAdapterFromDeviceName with STATUS_INVALID_PARAMETER (0xC000000D) for every adapter, while the
// GUID_DISPLAY_DEVICE_ARRIVAL path of the same device opens.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>       // NTSTATUS and NT_SUCCESS, which windows.h alone does not give a user-mode program
#include <setupapi.h>
#include <initguid.h>       // makes the DEFINE_GUID below emit the GUID itself, not just a declaration
#include <ntddvdeo.h>       // GUID_DISPLAY_DEVICE_ARRIVAL
#include <d3dkmthk.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../../../driver/kmd/bc250kmd_escape.h"     // shared with the driver, never copied

#define BC250_DEFAULT_HWID L"PCI\\VEN_1002&DEV_13FE"
#define BC250_SERVICE_KEY  L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd"
#define BC250_PARAMETERS   BC250_SERVICE_KEY L"\\Parameters"

// Mirrors enum BC250_STAGE in driver/kmd/bc250kmd.h; tools/win/bc250mon/test_stages.py watches that copy of
// the same table, and this one is checked against it by tools/win/bc250kmd_cli/test_stages.py.
static const struct { unsigned long Number; const char *Symbol, *Text; } g_Stages[] = {
    {  0, "StageNone",                      "nothing written yet" },
    { 10, "StageDriverEntry",               "DriverEntry" },
    { 20, "StageAddDevice",                 "add device" },
    { 30, "StageStartEnter",                "start: entered" },
    { 31, "StageStartGuardPassed",          "start: guard passed" },
    { 32, "StageStartDeviceInfo",           "start: device info" },
    { 33, "StageStartPostDisplayAcquired",  "start: post display acquired" },
    { 34, "StageStartFramebufferMapped",    "start: framebuffer mapped" },
    { 35, "StageStartMmioDone",             "start: register gate handled" },
    { 39, "StageStartDone",                 "start: done" },
    { 50, "StageFirstCommitVidPn",          "first CommitVidPn" },
    { 60, "StageFirstPresent",              "first present" },
    { 61, "StageFirstPresentDone",          "first present done" },
    { 70, "StageStopEnter",                 "stop: entered" },
    { 79, "StageStopDone",                  "stop: done" },
    { 90, "StageRefusedByGuard",            "refused by the guard" },
    { 91, "StageStartFailed",               "start failed" },
};

static const char *StageName(unsigned long stage)
{
    for (size_t i = 0; i < sizeof(g_Stages) / sizeof(g_Stages[0]); i++)
        if (g_Stages[i].Number == stage) return g_Stages[i].Text;
    return "unknown stage";
}

// The few NTSTATUS values this tool is likely to meet. Anything else is printed as a bare number, which is
// what matters: a hex status can be looked up, a guessed name cannot.
static const char *StatusName(NTSTATUS s)
{
    switch ((unsigned long)s) {
    case 0x00000000ul: return "STATUS_SUCCESS";
    case 0xC0000001ul: return "STATUS_UNSUCCESSFUL";
    case 0xC000000Dul: return "STATUS_INVALID_PARAMETER";
    case 0xC0000002ul: return "STATUS_NOT_IMPLEMENTED";
    case 0xC00000BBul: return "STATUS_NOT_SUPPORTED";
    case 0xC0000022ul: return "STATUS_ACCESS_DENIED";
    case 0xC00000A3ul: return "STATUS_DEVICE_NOT_READY";
    case 0xC0000008ul: return "STATUS_INVALID_HANDLE";
    case 0xC000000Ful: return "STATUS_NO_SUCH_FILE";
    case 0xC0000023ul: return "STATUS_BUFFER_TOO_SMALL";
    case 0xC00000E5ul: return "STATUS_INTERNAL_ERROR";
    case 0xC01E0102ul: return "STATUS_GRAPHICS_DRIVER_MISMATCH";
    case 0xC01E0200ul: return "STATUS_GRAPHICS_ADAPTER_WAS_RESET";
    default:           return "";
    }
}

static void PrintStatus(const char *call, NTSTATUS status)
{
    const char *name = StatusName(status);
    printf("%s -> 0x%08lX%s%s\n", call, (unsigned long)status, name[0] ? " " : "", name);
}

// ---- display adapters --------------------------------------------------------------------------------------

typedef struct {
    WCHAR InterfacePath[512];
    WCHAR HardwareId[512];      // the first line of the REG_MULTI_SZ, which is the most specific one
    WCHAR Description[256];
} BC250_ADAPTER;

// Fills up to Max adapters, returns how many were found, or -1 with a message on a SetupAPI failure.
static int FindAdapters(BC250_ADAPTER *out, int max)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DISPLAY_DEVICE_ARRIVAL, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA iface = { sizeof(iface) };
    union {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        UCHAR space[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 512 * sizeof(WCHAR)];
    } buffer;
    DWORD index = 0;
    int found = 0;

    if (set == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "SetupDiGetClassDevs(GUID_DISPLAY_DEVICE_ARRIVAL) failed, error %lu\n", GetLastError());
        return -1;
    }
    while (found < max && SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DISPLAY_DEVICE_ARRIVAL, index++, &iface)) {
        SP_DEVINFO_DATA info = { sizeof(info) };
        DWORD needed = 0;

        buffer.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, &buffer.detail, sizeof(buffer), &needed, &info)) {
            fprintf(stderr, "# SetupDiGetDeviceInterfaceDetail failed for interface %lu, error %lu\n", index - 1, GetLastError());
            continue;
        }
        memset(&out[found], 0, sizeof(out[found]));
        wcsncpy_s(out[found].InterfacePath, 512, buffer.detail.DevicePath, _TRUNCATE);
        // SPDRP_HARDWAREID is REG_MULTI_SZ, most specific id first: PCI\VEN_1002&DEV_13FE&SUBSYS_...&REV_..
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)out[found].HardwareId, sizeof(out[found].HardwareId), NULL))
            wcscpy_s(out[found].HardwareId, 512, L"(no hardware id)");
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, NULL,
                                               (PBYTE)out[found].Description, sizeof(out[found].Description), NULL))
            wcscpy_s(out[found].Description, 256, L"(no description)");
        found++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static int MatchesHardwareId(const WCHAR *hardwareId, const WCHAR *wanted)
{
    return _wcsnicmp(hardwareId, wanted, wcslen(wanted)) == 0;
}

static int ListAdapters(void)
{
    BC250_ADAPTER adapters[16];
    int count = FindAdapters(adapters, 16);

    if (count < 0) return 1;
    printf("# %d display adapter(s)\n", count);
    for (int i = 0; i < count; i++) {
        D3DKMT_OPENADAPTERFROMDEVICENAME open = { 0 };
        NTSTATUS status;

        printf("%d  %ls\n", i, adapters[i].Description);
        printf("   hardware id %ls\n", adapters[i].HardwareId);
        printf("   interface   %ls\n", adapters[i].InterfacePath);
        open.pDeviceName = adapters[i].InterfacePath;
        status = D3DKMTOpenAdapterFromDeviceName(&open);
        if (NT_SUCCESS(status)) {
            D3DKMT_CLOSEADAPTER close = { open.hAdapter };
            printf("   adapter     handle 0x%08lX luid %08lX-%08lX\n", (unsigned long)open.hAdapter,
                   (unsigned long)open.AdapterLuid.HighPart, open.AdapterLuid.LowPart);
            D3DKMTCloseAdapter(&close);
        } else {
            printf("   adapter     ");
            PrintStatus("D3DKMTOpenAdapterFromDeviceName", status);
        }
    }
    return count > 0 ? 0 : 1;
}

// ---- info: the escape --------------------------------------------------------------------------------------

static int Info(const WCHAR *wantedId)
{
    BC250_ADAPTER adapters[16];
    int count = FindAdapters(adapters, 16);
    int chosen = -1;
    D3DKMT_OPENADAPTERFROMDEVICENAME open = { 0 };
    D3DKMT_CLOSEADAPTER close = { 0 };
    D3DKMT_ESCAPE escape = { 0 };
    BC250_ESCAPE data;
    NTSTATUS status;

    if (count < 0) return 1;
    for (int i = 0; i < count && chosen < 0; i++)
        if (MatchesHardwareId(adapters[i].HardwareId, wantedId)) chosen = i;

    if (chosen < 0) {
        fprintf(stderr, "no display adapter with hardware id %ls among the %d present:\n", wantedId, count);
        for (int i = 0; i < count; i++) fprintf(stderr, "  %ls  (%ls)\n", adapters[i].HardwareId, adapters[i].Description);
        return 1;
    }
    printf("# adapter    %ls\n", adapters[chosen].Description);
    printf("# hardware   %ls\n", adapters[chosen].HardwareId);
    printf("# interface  %ls\n", adapters[chosen].InterfacePath);

    open.pDeviceName = adapters[chosen].InterfacePath;
    status = D3DKMTOpenAdapterFromDeviceName(&open);
    PrintStatus("D3DKMTOpenAdapterFromDeviceName", status);
    if (!NT_SUCCESS(status)) return 1;
    printf("# adapter    handle 0x%08lX luid %08lX-%08lX\n", (unsigned long)open.hAdapter,
           (unsigned long)open.AdapterLuid.HighPart, open.AdapterLuid.LowPart);

    memset(&data, 0, sizeof(data));
    data.Magic = BC250_ESCAPE_MAGIC;
    data.Command = BC250_ESCAPE_GET_INFO;

    escape.hAdapter = open.hAdapter;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;      // the only type a miniport's DxgkDdiEscape sees unchanged
    escape.Flags.Value = 0;                         // no HardwareAccess: M3 touches no register
    escape.pPrivateDriverData = &data;
    escape.PrivateDriverDataSize = sizeof(data);

    status = D3DKMTEscape(&escape);
    PrintStatus("D3DKMTEscape(D3DKMT_ESCAPE_DRIVERPRIVATE, BC250_ESCAPE_GET_INFO)", status);

    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);

    if (!NT_SUCCESS(status)) {
        // Say what was seen, not what it means. Against a driver with no private escape the call is expected
        // to fail; against bc250kmd, which does fill DxgkDdiEscape, the status decides ADR 0006's open
        // question about the control channel for M4. Measured statuses are collected in docs/facts.md.
        printf("# the escape failed. Expected against a driver that has no private escape of ours;\n"
               "# against bc250kmd this status is the answer to ADR 0006's question about the control channel.\n");
        return 1;
    }
    if (data.Magic != BC250_ESCAPE_MAGIC) {
        printf("# the escape returned success but the buffer came back changed (magic 0x%08lX): not our driver\n", data.Magic);
        return 1;
    }
    if (data.Status != 0) {
        printf("# the driver refused the command (Status %lu)\n", data.Status);
        return 1;
    }
    printf("version      0x%08lX (milestone %lu revision %lu)\n", data.Version, data.Version >> 16, data.Version & 0xFFFFu);
    printf("last stage   %lu %s\n", data.LastStage, StageName(data.LastStage));
    printf("mode         %lux%lu pitch %lu format %lu (%s)\n", data.Width, data.Height, data.Pitch, data.ColorFormat,
           (data.Flags & BC250_ESCAPE_FLAG_FULL_WDDM) ? "FULL WDDM TABLE" : "display-only");
    printf("presents     %lu\n", data.Presents);
    printf("counters     blits %lu, flips %lu (the full table's own present counters; both stay 0 in display-only mode)\n",
           data.Reserved[0], data.Reserved[1]);
    printf("gates        mmio %s, mmio writes %s, vram %s, vram writes %s, gart %s, psp %s, gfx %s, ih %s\n",
           (data.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "open" : "closed", (data.Flags & BC250_ESCAPE_FLAG_MMIO_WRITE) ? "on" : "off",
           (data.Flags & BC250_ESCAPE_FLAG_VRAM) ? "open" : "closed", (data.Flags & BC250_ESCAPE_FLAG_VRAM_WRITE) ? "on" : "off",
           (data.Flags & BC250_ESCAPE_FLAG_GART) ? "open" : "closed", (data.Flags & BC250_ESCAPE_FLAG_PSP) ? "open" : "closed",
           (data.Flags & BC250_ESCAPE_FLAG_GFX) ? "open" : "closed", (data.Flags & BC250_ESCAPE_FLAG_IH) ? "open" : "closed");
    return 0;
}

// ---- read / write: registers through the escape (ADR 0007) ----------------------------------------------------
//
// Offsets are BAR5 byte offsets and come from tools/regcalc (on the target: bc250rd's reglist.txt), never from
// memory. The driver checks them against its own generated tables, so a wrong one is refused, not executed.

static int SendEscapeFlags(const WCHAR *wantedId, void *data, unsigned size, NTSTATUS *result, int hardwareAccess)
{
    BC250_ADAPTER adapters[16];
    int count = FindAdapters(adapters, 16);
    int chosen = -1;
    D3DKMT_OPENADAPTERFROMDEVICENAME open = { 0 };
    D3DKMT_CLOSEADAPTER close = { 0 };
    D3DKMT_ESCAPE escape = { 0 };

    for (int i = 0; i < count && chosen < 0; i++)
        if (MatchesHardwareId(adapters[i].HardwareId, wantedId)) chosen = i;
    if (chosen < 0) { fprintf(stderr, "no display adapter with hardware id %ls\n", wantedId); return 1; }

    open.pDeviceName = adapters[chosen].InterfacePath;
    *result = D3DKMTOpenAdapterFromDeviceName(&open);
    if (!NT_SUCCESS(*result)) { PrintStatus("D3DKMTOpenAdapterFromDeviceName", *result); return 1; }

    escape.hAdapter = open.hAdapter;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.HardwareAccess = hardwareAccess ? 1u : 0u;
    escape.Flags.NoAdapterSynchronization = hardwareAccess ? 0u : 1u;
    escape.pPrivateDriverData = data;
    escape.PrivateDriverDataSize = size;
    *result = D3DKMTEscape(&escape);

    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
    return 0;
}

static int SendEscape(const WCHAR *wantedId, void *data, unsigned size, NTSTATUS *result)
{
    return SendEscapeFlags(wantedId,data,size,result,1);
}

#ifdef BC250_CONTROL_DLL
#define BC250_CONTROL_API __declspec(dllexport)
#else
#define BC250_CONTROL_API
#endif
// Shared implementation for CLI and direct monitor P/Invoke, no subprocess or
// raw bc250rd fallback. A new adapter handle each call survives PnP replacement.
BC250_CONTROL_API LONG WINAPI Bc250ClockControl(ULONG op,ULONG mhz,ULONG mv,
    BC250_ESCAPE_CLOCK* data,ULONG bytes)
{
    NTSTATUS status=(NTSTATUS)0xC000000E; // STATUS_NO_SUCH_DEVICE before adapter lookup
    if(!data || bytes!=sizeof(*data) || (op!=BC250_CLOCK_OP_READ && op!=BC250_CLOCK_OP_SET))
        return (LONG)0xC000000D; // STATUS_INVALID_PARAMETER
    memset(data,0,sizeof(*data));
    data->Magic=BC250_ESCAPE_MAGIC;data->Command=BC250_ESCAPE_RUN_CLOCK;
    data->AbiVersion=BC250_CLOCK_ABI;data->Op=op;
    data->RequestedMHz=mhz;data->RequestedMv=mv;
    if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),&status,op==BC250_CLOCK_OP_SET))return status;
    if(!NT_SUCCESS(status))return status;
    if(data->Status==BC250_ESCAPE_STATUS_UNKNOWN_COMMAND)return (LONG)0xC00000BB;
    if(data->Status!=BC250_ESCAPE_STATUS_DONE || !data->Ready)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    return 0;
}

// READ is an adapter-owned software snapshot, never a hardware-idling diagnostic.
BC250_CONTROL_API LONG WINAPI Bc250StartHealth(ULONG op,ULONGLONG generation,ULONGLONG epoch,
    BC250_ESCAPE_START_HEALTH* data,ULONG bytes)
{
    NTSTATUS status=(NTSTATUS)0xC000000E;
    typedef char HealthAbiSizeCheck[(sizeof(BC250_ESCAPE_START_HEALTH)==96)?1:-1];
    (void)sizeof(HealthAbiSizeCheck);
    if(!data || bytes!=sizeof(*data) ||
       (op!=BC250_START_HEALTH_READ && op!=BC250_START_HEALTH_CONFIRM))
        return (LONG)0xC000000D;
    memset(data,0,sizeof(*data));
    data->Magic=BC250_ESCAPE_MAGIC;data->Command=BC250_ESCAPE_RUN_START_HEALTH;
    data->Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion=BC250_START_HEALTH_ABI;data->Op=op;
    data->ExpectedGeneration=generation;data->ExpectedEpoch=epoch;
    if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),&status,op==BC250_START_HEALTH_CONFIRM))return status;
    if(!NT_SUCCESS(status))return status;
    if(data->Status==BC250_ESCAPE_STATUS_UNKNOWN_COMMAND)return (LONG)0xC00000BB;
    if(data->Status!=BC250_ESCAPE_STATUS_DONE || data->NtStatus!=0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if(data->Magic!=BC250_ESCAPE_MAGIC || data->Command!=BC250_ESCAPE_RUN_START_HEALTH ||
       data->AbiVersion!=BC250_START_HEALTH_ABI || data->Op!=op)
        return (LONG)0xC000000D;
    if(op==BC250_START_HEALTH_CONFIRM &&
       (data->Generation!=generation || data->Epoch!=epoch || (data->Flags&(BC250_START_HEALTH_REQUIRED|BC250_START_HEALTH_CONFIRMED))!=(BC250_START_HEALTH_REQUIRED|BC250_START_HEALTH_CONFIRMED)))
        return (LONG)0xC00000A3;
    return 0;
}

static int StartHealth(int argc,wchar_t** argv)
{
    BC250_ESCAPE_START_HEALTH data;
    ULONG op=BC250_START_HEALTH_READ;
    ULONGLONG generation=0,epoch=0;
    WCHAR* end;
    LONG status;
    if(argc==5 && !_wcsicmp(argv[2],L"confirm")) {
        op=BC250_START_HEALTH_CONFIRM;
        generation=_wcstoui64(argv[3],&end,10);if(*end || !generation || argv[3][0]==L'-')return 2;
        epoch=_wcstoui64(argv[4],&end,10);if(*end || !epoch || argv[4][0]==L'-')return 2;
    } else if(argc!=3 || _wcsicmp(argv[2],L"read"))return 2;
    status=Bc250StartHealth(op,generation,epoch,&data,sizeof(data));
    if(status<0){PrintStatus("KMD start health",status);return 1;}
    printf("health abi=%lu version=0x%08lX flags=%lu generation=%llu epoch=%llu completed=%llu age_ms=%llu ready_ms=%llu\n",
        data.AbiVersion,data.Version,data.Flags,data.Generation,data.Epoch,data.Completed,
        data.LastCompletionAgeMs,data.ReadyAgeMs);
    return 0;
}

static int Clock(int argc,wchar_t** argv)
{
    BC250_ESCAPE_CLOCK data;
    ULONG op=BC250_CLOCK_OP_READ,mhz=0,mv=0;
    WCHAR* end;
    LONG status;
    if(argc==5 && !_wcsicmp(argv[2],L"set")) {
        op=BC250_CLOCK_OP_SET;
        mhz=wcstoul(argv[3],&end,10);if(*end || argv[3][0]==L'-')return 2;
        mv=wcstoul(argv[4],&end,10);if(*end || argv[4][0]==L'-')return 2;
    } else if(argc!=3 || _wcsicmp(argv[2],L"read"))return 2;
    status=Bc250ClockControl(op,mhz,mv,&data,sizeof(data));
    if(status<0) { PrintStatus("KMD clock",status);return 1; }
    printf("clock backend=kmd-smu abi=%lu MHz=%lu VID=%lu temperature_mc=%ld ready=%lu\n",
        data.AbiVersion,data.ObservedMHz,data.ObservedVid,data.TemperatureMc,data.Ready);
    if(op==BC250_CLOCK_OP_SET)
        printf("clock request=%luMHz/%lumV initial=%luMHz/VID%lu expected_vid=%lu voltage_staged=%lu\n",
            mhz,mv,data.InitialMHz,data.InitialVid,data.ExpectedVid,data.VoltageStaged);
    return 0;
}

static int Register(int write, const WCHAR *offsetText, const WCHAR *valueText)
{
    BC250_ESCAPE data;
    NTSTATUS status;
    WCHAR *end;

    memset(&data, 0, sizeof(data));
    data.Magic = BC250_ESCAPE_MAGIC;
    data.Command = write ? BC250_ESCAPE_WRITE_REG : BC250_ESCAPE_READ_REG;
    data.RegOffset = wcstoul(offsetText, &end, 16);
    if (*end) { fprintf(stderr, "offset %ls is not a hex number\n", offsetText); return 2; }
    if (write) {
        data.RegValue = wcstoul(valueText, &end, 16);
        if (*end) { fprintf(stderr, "value %ls is not a hex number\n", valueText); return 2; }
    }
    if (SendEscape(BC250_DEFAULT_HWID, &data, sizeof(data), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }

    if (data.Status == BC250_ESCAPE_STATUS_DONE) {
        printf("%s 0x%05lx %08lX\n", write ? "wrote" : "read", data.RegOffset, data.RegValue);
        return 0;
    }
    printf("refused 0x%05lx: %s (driver status %lu, NTSTATUS 0x%08lX %s; mmio %s, writes %s)\n", data.RegOffset,
           data.Status == BC250_ESCAPE_STATUS_NOT_ADMIN ? "caller is not an administrator" :
           data.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND ? "this driver build has no register commands" : "see NTSTATUS",
           data.Status, data.NtStatus, StatusName((NTSTATUS)data.NtStatus),
           (data.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "not mapped (gate closed)",
           (data.Flags & BC250_ESCAPE_FLAG_MMIO_WRITE) ? "on" : "off");
    return 3;
}

// ---- memory / vread / vwrite / vcompare: the VRAM carve-out through the escape (experiment E08) ---------------
//
// Two independent ways to the same byte: "phys" is the carve-out's system physical address, "bar0" the PCI
// aperture. The driver accepts writes in its one-page test window only.

static int MemoryEscape(BC250_ESCAPE_MEMORY *data, unsigned long command, unsigned long path, unsigned long long offset,
                        unsigned long value)
{
    NTSTATUS status;

    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = command;
    data->Path = path;
    data->Offset = offset;
    data->Value = value;
    if (SendEscape(BC250_DEFAULT_HWID, data, sizeof(*data), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (data->Status == BC250_ESCAPE_STATUS_DONE) return 0;
    printf("refused: %s (driver status %lu, NTSTATUS 0x%08lX %s; vram %s, writes %s)\n",
           data->Status == BC250_ESCAPE_STATUS_NOT_ADMIN ? "caller is not an administrator" :
           data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND ? "this driver build has no memory commands" : "see NTSTATUS",
           data->Status, data->NtStatus, StatusName((NTSTATUS)data->NtStatus),
           (data->Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "not identified (gate closed)",
           (data->Flags & BC250_ESCAPE_FLAG_VRAM_WRITE) ? "on" : "off");
    return 3;
}

static int Memory(void)
{
    BC250_ESCAPE_MEMORY m;
    int r = MemoryEscape(&m, BC250_ESCAPE_GET_MEMORY, 0, 0, 0);

    if (r) return r;
    printf("framebuffer  0x%llX + 0x%llX\n", m.FramebufferPhysical, m.FramebufferLength);
    printf("bar0         0x%llX + 0x%llX\n", m.Bar0Physical, m.Bar0Length);
    printf("vram         0x%llX + 0x%llX, MC base 0x%llX\n", m.VramPhysical, m.VramLength, m.VramMcBase);
    printf("test window  0x%llX + 0x%llX\n", m.TestOffset, m.TestLength);
    printf("gates        vram %s, vram writes %s\n", (m.Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed",
           (m.Flags & BC250_ESCAPE_FLAG_VRAM_WRITE) ? "on" : "off");
    return 0;
}

static int ParsePath(const WCHAR *text, unsigned long *path)
{
    if (!_wcsicmp(text, L"phys")) { *path = BC250_VRAM_PATH_PHYSICAL; return 1; }
    if (!_wcsicmp(text, L"bar0")) { *path = BC250_VRAM_PATH_BAR0; return 1; }
    fprintf(stderr, "path is phys or bar0, not %ls\n", text);
    return 0;
}

static int VramWord(int write, const WCHAR *pathText, const WCHAR *offsetText, const WCHAR *valueText)
{
    BC250_ESCAPE_MEMORY m;
    unsigned long path, value = 0;
    unsigned long long offset;
    WCHAR *end;
    int r;

    if (!ParsePath(pathText, &path)) return 2;
    offset = _wcstoui64(offsetText, &end, 16);
    if (*end) { fprintf(stderr, "offset %ls is not a hex number\n", offsetText); return 2; }
    if (write) {
        value = wcstoul(valueText, &end, 16);
        if (*end) { fprintf(stderr, "value %ls is not a hex number\n", valueText); return 2; }
    }
    r = MemoryEscape(&m, write ? BC250_ESCAPE_VRAM_WRITE : BC250_ESCAPE_VRAM_READ, path, offset, value);
    if (r) return r;
    printf("%s %ls 0x%llX %08lX\n", write ? "wrote" : "read", pathText, offset, m.Value);
    return 0;
}

// Read <count> words at <offset> through both paths and say whether they are the same memory.
static int VramCompare(const WCHAR *offsetText, const WCHAR *countText)
{
    BC250_ESCAPE_MEMORY a, b;
    unsigned long long offset;
    unsigned long count, same = 0, nonzero = 0;
    WCHAR *end;
    int r;

    offset = _wcstoui64(offsetText, &end, 16);
    if (*end) { fprintf(stderr, "offset %ls is not a hex number\n", offsetText); return 2; }
    count = wcstoul(countText, &end, 10);
    if (*end || count == 0 || count > 1024) { fprintf(stderr, "count is 1..1024\n"); return 2; }
    for (unsigned long i = 0; i < count; i++) {
        if ((r = MemoryEscape(&a, BC250_ESCAPE_VRAM_READ, BC250_VRAM_PATH_PHYSICAL, offset + 4ull * i, 0)) != 0) return r;
        if ((r = MemoryEscape(&b, BC250_ESCAPE_VRAM_READ, BC250_VRAM_PATH_BAR0, offset + 4ull * i, 0)) != 0) return r;
        same += a.Value == b.Value;
        nonzero += a.Value != 0;
        if (i < 8 || a.Value != b.Value) printf("0x%llX  phys %08lX  bar0 %08lX%s\n", offset + 4ull * i, a.Value, b.Value,
                                                 a.Value == b.Value ? "" : "   DIFFERENT");
    }
    printf("compared %lu words at 0x%llX: %lu same, %lu different, %lu nonzero\n", count, offset, same, count - same, nonzero);
    return same == count ? 0 : 4;
}

// One page table page: 512 entries of 8 bytes at <offset> (from VRAM byte 0), read through the physical path; the
// non-zero ones are printed. E18's witness walks VidMm's page tables with this.
static int VramTable(const WCHAR *offsetText)
{
    BC250_ESCAPE_MEMORY lo, hi;
    unsigned long long offset;
    unsigned long nonzero = 0;
    WCHAR *end;
    int r;

    offset = _wcstoui64(offsetText, &end, 16);
    if (*end || (offset & 0xFFF)) { fprintf(stderr, "offset %ls is not a page-aligned hex number\n", offsetText); return 2; }
    for (unsigned long i = 0; i < 512; i++) {
        unsigned long long entry;

        if ((r = MemoryEscape(&lo, BC250_ESCAPE_VRAM_READ, BC250_VRAM_PATH_PHYSICAL, offset + 8ull * i, 0)) != 0) return r;
        if ((r = MemoryEscape(&hi, BC250_ESCAPE_VRAM_READ, BC250_VRAM_PATH_PHYSICAL, offset + 8ull * i + 4, 0)) != 0) return r;
        entry = ((unsigned long long)hi.Value << 32) | lo.Value;
        if (entry != 0) { printf("%3lu 0x%016llX\n", i, entry); nonzero++; }
    }
    printf("table at 0x%llX: %lu of 512 entries nonzero\n", offset, nonzero);
    return 0;
}

// ---- gart: the M4 sequence (experiment E09) ---------------------------------------------------------------------
//
// plan executes no write: the driver runs AMD's imported sequence against the real registers and returns what
// it would write. enable runs it; restore writes the firmware's state back. Output: one "W <offset> <value>"
// line per write, which experiments/E09-*/compare_plan.py compares with amdgpu's trace.

static int Gart(const WCHAR *opText)
{
    static BC250_ESCAPE_GART g;
    NTSTATUS status;

    memset(&g, 0, sizeof(g));
    g.Magic = BC250_ESCAPE_MAGIC;
    g.Command = BC250_ESCAPE_RUN_GART;
    if (!_wcsicmp(opText, L"plan")) g.Op = BC250_GART_OP_PLAN;
    else if (!_wcsicmp(opText, L"enable")) g.Op = BC250_GART_OP_ENABLE;
    else if (!_wcsicmp(opText, L"restore")) g.Op = BC250_GART_OP_RESTORE;
    else { fprintf(stderr, "gart plan | enable | restore, not %ls\n", opText); return 2; }

    if (SendEscape(BC250_DEFAULT_HWID, &g, sizeof(g), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (g.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (g.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no GART command\n"); return 3; }

    printf("gart %ls: %s, NTSTATUS 0x%08lX %s, sequence result %ld, %lu writes%s\n", opText,
           g.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", g.NtStatus, StatusName((NTSTATUS)g.NtStatus), g.Result,
           g.WriteCount, g.Op == BC250_GART_OP_PLAN ? " planned, none executed" : " executed");
    printf("gates        mmio %s, vram %s, gart %s\n", (g.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed",
           (g.Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed", (g.Flags & BC250_ESCAPE_FLAG_GART) ? "open" : "closed");
    if (g.FaultOffset) printf("fault        register 0x%05lX was refused by the driver's table; the sequence stopped there\n", g.FaultOffset);
    printf("state        %s, snapshot %s\n", (g.State & BC250_GART_STATE_ENABLED) ? "ENABLED" : "not enabled",
           (g.State & BC250_GART_STATE_SNAPSHOT) ? "held" : "none");
    printf("table        physical 0x%llX, MC 0x%llX\n", g.TablePhysical, g.TableMc);
    printf("scratch      MC 0x%llX\n", g.ScratchMc);
    printf("dummy page   physical 0x%llX\n", g.DummyPhysical);
    for (unsigned long i = 0; i < g.WriteCount && i < BC250_GART_MAX_WRITES; i++)
        printf("W 0x%05lX %08lX\n", g.Writes[i].Offset, g.Writes[i].Value);
    return g.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- psp: firmware through the PSP (experiment E10) ---------------------------------------------------------------
//
// plan executes no write and touches no VRAM: the driver reads the firmware files, lays the images out and runs
// AMD's ring create against the real registers without writing. load does it (needs gart enable first); unload is
// DESTROY_TMR plus ring stop. Output: "W <offset> <value>" per register write, "C ..." per PSP command.

static int Psp(const WCHAR *opText)
{
    static BC250_ESCAPE_PSP p;
    NTSTATUS status;

    memset(&p, 0, sizeof(p));
    p.Magic = BC250_ESCAPE_MAGIC;
    p.Command = BC250_ESCAPE_RUN_PSP;
    if (!_wcsicmp(opText, L"plan")) p.Op = BC250_PSP_OP_PLAN;
    else if (!_wcsicmp(opText, L"load")) p.Op = BC250_PSP_OP_LOAD;
    else if (!_wcsicmp(opText, L"unload")) p.Op = BC250_PSP_OP_UNLOAD;
    else { fprintf(stderr, "psp plan | load | unload, not %ls\n", opText); return 2; }

    if (SendEscape(BC250_DEFAULT_HWID, &p, sizeof(p), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (p.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (p.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no PSP command\n"); return 3; }

    printf("psp %ls: %s, NTSTATUS 0x%08lX %s, sequence result %ld, %lu register writes%s, %lu of %lu commands\n", opText,
           p.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", p.NtStatus, StatusName((NTSTATUS)p.NtStatus), p.Result,
           p.WriteCount, p.Op == BC250_PSP_OP_PLAN ? " planned, none executed" : " executed", p.CommandsDone, p.CommandCount);
    printf("gates        mmio %s, vram %s, gart %s, psp %s\n", (p.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed",
           (p.Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed", (p.Flags & BC250_ESCAPE_FLAG_GART) ? "open" : "closed",
           (p.Flags & BC250_ESCAPE_FLAG_PSP) ? "open" : "closed");
    if (p.FaultOffset) printf("fault        register 0x%05lX was refused by the driver's table; the sequence stopped there\n", p.FaultOffset);
    printf("state        gart %s, psp ring %s, tmr %s\n", (p.State & BC250_PSP_STATE_GART) ? "enabled" : "not enabled",
           (p.State & BC250_PSP_STATE_RING) ? "KNOWN TO THE PSP" : "none", (p.State & BC250_PSP_STATE_TMR) ? "KNOWN TO THE PSP" : "none");
    printf("ring         MC 0x%llX, command buffer MC 0x%llX, fence MC 0x%llX\n", p.RingMc, p.CommandMc, p.FenceMc);
    printf("tmr          MC 0x%llX, physical 0x%llX\n", p.TmrMc, p.TmrPhysical);
    printf("staging      MC 0x%llX, 0x%lX bytes used\n", p.StagingMc, p.StagingUsed);
    for (unsigned long i = 0; i < p.WriteCount && i < BC250_PSP_MAX_WRITES; i++)
        printf("W 0x%05lX %08lX\n", p.Writes[i].Offset, p.Writes[i].Value);
    for (unsigned long i = 0; i < p.CommandCount && i < BC250_PSP_MAX_COMMANDS; i++)
    {
        const BC250_ESCAPE_PSP_COMMAND *c = &p.Commands[i];
        printf("C %2lu id %lu type %2lu size %7lu mc 0x%llX %s rc %ld status 0x%08lX us %lu tmr 0x%llX\n", i + 1, c->CommandId,
               c->FirmwareType, c->Size, c->McAddress, i < p.CommandsDone ? "submitted" : "planned  ", c->Result, c->PspStatus,
               c->Microseconds, c->TmrAddress);
    }
    return p.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- gfx: RLC, CP, KIQ, queues, ring tests, SDMA (experiment E11) ------------------------------------------------------
//
// plan <stage> runs stages 1..stage against the real registers without executing a register or doorbell write;
// run <stage> executes the stages not yet done, up to <stage>; fini halts the engines and gives the memory back.
// Output: "S ..." per stage, "W <offset> <value>" per register write, "D <index> <value> after <n>" per doorbell.

static const char *GfxStageName(unsigned long stage)
{
    static const char *names[] = { "?", "doorbell aperture", "golden registers", "GRBM CAM probe", "constants", "RLC", "CP", "SDMA", "interrupt sources" };

    return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "?";
}

static int Gfx(const WCHAR *opText, const WCHAR *stageText)
{
    static BC250_ESCAPE_GFX g;
    NTSTATUS status;

    memset(&g, 0, sizeof(g));
    g.Magic = BC250_ESCAPE_MAGIC;
    g.Command = BC250_ESCAPE_RUN_GFX;
    if (!_wcsicmp(opText, L"plan")) g.Op = BC250_GFX_OP_PLAN;
    else if (!_wcsicmp(opText, L"run")) g.Op = BC250_GFX_OP_RUN;
    else if (!_wcsicmp(opText, L"fini")) g.Op = BC250_GFX_OP_FINI;
    else if (!_wcsicmp(opText, L"state")) g.Op = BC250_GFX_OP_STATE;
    else { fprintf(stderr, "gfx plan <stage> | run <stage> | fini | state, not %ls\n", opText); return 2; }
    if (g.Op <= BC250_GFX_OP_RUN)
    {
        if (stageText == NULL) { fprintf(stderr, "gfx %ls needs the last stage to run (1..8)\n", opText); return 2; }
        g.LastStage = wcstoul(stageText, NULL, 0);
    }

    if (SendEscape(BC250_DEFAULT_HWID, &g, sizeof(g), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (g.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (g.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no GFX command\n"); return 3; }

    printf("gfx %ls to stage %lu: %s, NTSTATUS 0x%08lX %s, result %ld%s, %lu register writes and %lu doorbells%s\n", opText, g.LastStage,
           g.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", g.NtStatus, StatusName((NTSTATUS)g.NtStatus), g.Result,
           g.FailedStage ? " (a stage failed)" : "", g.WriteCount, g.DoorbellCount,
           g.Op == BC250_GFX_OP_PLAN ? " planned, none executed" : " executed");
    printf("gates        mmio %s, vram %s, gart %s, psp %s, gfx %s\n", (g.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed",
           (g.Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed", (g.Flags & BC250_ESCAPE_FLAG_GART) ? "open" : "closed",
           (g.Flags & BC250_ESCAPE_FLAG_PSP) ? "open" : "closed", (g.Flags & BC250_ESCAPE_FLAG_GFX) ? "open" : "closed");
    if (g.FaultOffset) printf("fault        0x%08lX was refused by the driver's table; the sequence stopped there\n", g.FaultOffset);
    printf("state        stages done on the hardware: %lu (%s)\n", g.StagesDone, g.StagesDone ? GfxStageName(g.StagesDone) : "none");
    printf("memory       VRAM 0x%lX bytes, GTT 0x%lX bytes held\n", g.VramBytes, g.GttBytes);
    for (unsigned long i = 0; i < g.StageCount && i < BC250_GFX_MAX_STAGES; i++)
        printf("S %lu %-18s rc %ld first write %lu us %lu%s\n", g.Stages[i].Stage, GfxStageName(g.Stages[i].Stage), g.Stages[i].Result,
               g.Stages[i].FirstWrite, g.Stages[i].Microseconds, g.Stages[i].Stage == g.FailedStage ? "  FAILED" : "");
    for (unsigned long i = 0; i < g.WriteCount && i < BC250_GFX_MAX_WRITES; i++)
        printf("W 0x%05lX %08lX\n", g.Writes[i].Offset, g.Writes[i].Value);
    for (unsigned long i = 0; i < g.DoorbellCount && i < BC250_GFX_MAX_DOORBELLS; i++)
        printf("D 0x%03lX %016llX after %lu\n", g.Doorbells[i].Index, g.Doorbells[i].Value, g.Doorbells[i].AfterWrite);
    return g.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- ih: the interrupt controller's ring (BC250_ESCAPE_RUN_IH) ---------------------------------------------------

static int Ih(const WCHAR *opText)
{
    static BC250_ESCAPE_IH h;
    NTSTATUS status;

    memset(&h, 0, sizeof(h));
    h.Magic = BC250_ESCAPE_MAGIC;
    h.Command = BC250_ESCAPE_RUN_IH;
    if (!_wcsicmp(opText, L"plan")) h.Op = BC250_IH_OP_PLAN;
    else if (!_wcsicmp(opText, L"init")) h.Op = BC250_IH_OP_INIT;
    else if (!_wcsicmp(opText, L"fini")) h.Op = BC250_IH_OP_FINI;
    else if (!_wcsicmp(opText, L"state")) h.Op = BC250_IH_OP_STATE;
    else { fprintf(stderr, "ih plan | init | fini | state, not %ls\n", opText); return 2; }

    if (SendEscape(BC250_DEFAULT_HWID, &h, sizeof(h), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (h.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (h.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no IH command\n"); return 3; }

    printf("ih %ls: %s, NTSTATUS 0x%08lX %s, result %ld, %lu register writes%s\n", opText,
           h.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", h.NtStatus, StatusName((NTSTATUS)h.NtStatus), h.Result, h.WriteCount,
           h.Op == BC250_IH_OP_PLAN ? " planned, none executed" : " executed");
    printf("gates        mmio %s, gart %s, psp %s, gfx %s, ih %s\n", (h.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed",
           (h.Flags & BC250_ESCAPE_FLAG_GART) ? "open" : "closed", (h.Flags & BC250_ESCAPE_FLAG_PSP) ? "open" : "closed",
           (h.Flags & BC250_ESCAPE_FLAG_GFX) ? "open" : "closed", (h.Flags & BC250_ESCAPE_FLAG_IH) ? "open" : "closed");
    if (h.FaultOffset) printf("fault        0x%08lX was refused by the driver's table; the sequence stopped there\n", h.FaultOffset);
    printf("windows      interrupt resource: %s, vector 0x%lX; interrupt routine called %lu times since the device started, last message %lu\n",
           h.InterruptIsMessage ? "message (MSI)" : "line", h.InterruptVector, h.InterruptCount, h.LastMessageNumber);
    printf("ring         %s, %lu interrupts taken as ours, %lu DPCs, %lu vectors consumed, %lu overflows, rptr 0x%lX wptr 0x%lX\n",
           h.Active ? "ENABLED" : "off", h.OurInterrupts, h.DpcCount, h.EntryCount, h.OverflowCount, h.Rptr, h.Wptr);
    printf("status       IH_STATUS: idle %s, input_idle %s, bif_interrupt_line %s (docs/design/vsync-interrupt-route.md:\n"
           "             input_idle clear would mean some client has something pending at IH's own input; bif_interrupt_line\n"
           "             set would mean NBIO/BIF itself sees the host interrupt line asserted right now)\n",
           h.Idle ? "yes" : "no", h.InputIdle ? "yes" : "no", h.BifInterruptLine ? "SET" : "clear");
    for (unsigned long i = 0; i < h.KindCount && i < BC250_IH_MAX_KINDS; i++)
        printf("K client %lu source %lu count %lu\n", h.Kinds[i].ClientId, h.Kinds[i].SourceId, h.Kinds[i].Count);
    for (unsigned long i = 0; i < h.LastCount && i < BC250_IH_MAX_LAST; i++)
        printf("V client %lu source %lu ring %lu vmid %lu pasid %lu data %08lX %08lX %08lX %08lX ts %llu\n", h.Last[i].ClientId,
               h.Last[i].SourceId, h.Last[i].RingId, h.Last[i].VmId, h.Last[i].Pasid, h.Last[i].SrcData[0], h.Last[i].SrcData[1],
               h.Last[i].SrcData[2], h.Last[i].SrcData[3], h.Last[i].Timestamp);
    for (unsigned long i = 0; i < h.WriteCount && i < BC250_IH_MAX_WRITES; i++)
        printf("W 0x%05lX %08lX\n", h.Writes[i].Offset, h.Writes[i].Value);
    return h.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- fence: fences on one ring (BC250_ESCAPE_RUN_FENCE) ----------------------------------------------------------------
//
// fence c0..c7 <groups> dispatch: libdrm's gfx10 memset shader on that compute ring (the M6 exit criterion), 1..16 workgroups.
// fence gfx 1 ib: the driver builds the ring test as an indirect buffer in a GTT page of its own and submits it at VMID 0
// (ADR 0008 step C3). It reports whether the scratch register took the value, which is what says the CP fetched the buffer.
// fence gfx|c0..c7|kiq|s0|s1 [count] [noint|test]: emit <count> fences one after the other and poll each value in memory
// (test, SDMA only: the ring test, one WRITE_LINEAR of 0xDEADBEEF, instead). With the
// IH ring up and stage 8 done each one also raises an end-of-pipe interrupt (`ih state` counts them); noint is the
// control: the same packet without the interrupt bit.

static void FencePrint(const BC250_ESCAPE_FENCE *f, const WCHAR *what)
{
    printf("fence %ls x%lu%s: %s, NTSTATUS 0x%08lX %s, result %ld\n", what, f->Count,
           f->Interrupt == BC250_FENCE_MODE_RING_TEST ? " as a ring test" :
           f->Interrupt == BC250_FENCE_MODE_IB || f->Interrupt == BC250_FENCE_MODE_IB_AT ? " as an indirect buffer" :
           f->Interrupt ? "" : " without the interrupt bit",
           f->Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", f->NtStatus, StatusName((NTSTATUS)f->NtStatus), f->Result);
    if (f->FaultOffset) printf("fault        0x%08lX was refused by the driver's table; the sequence stopped there\n", f->FaultOffset);
    printf("fences       %lu of %lu values read back, %lu doorbells; last emitted 0x%lX, slot holds 0x%lX\n", f->Completed, f->Count,
           f->DoorbellCount, f->LastSeq, f->LastValue);
    printf("time         %lu us in all, slowest single fence %lu us\n", f->Microseconds, f->SlowestMicroseconds);
    if (f->Interrupt == BC250_FENCE_MODE_DISPATCH)
        printf("dispatch     %lu workgroup(s) of 64 threads, fill 0x%08X: check %ld%s, first wrong dword at byte 0x%lX\n", f->Count,
               BC250_DISPATCH_FILL, f->DispatchCheck, f->DispatchCheck == 0 && f->Completed ? " (every dword as asked)" : "", f->DispatchBadOffset);
    if (f->Interrupt == BC250_FENCE_MODE_IB || f->Interrupt == BC250_FENCE_MODE_IB_AT)
        printf("ib           0x%llX, %lu dwords, VMID %lu, root 0x%llX: %s\n", f->IbAddress, f->Dwords, f->Vmid, f->RootPhysical,
               f->Interrupt == BC250_FENCE_MODE_IB ? (f->IbFetched ? "FETCHED (the scratch register took the value)"
                                                                   : "not fetched (the register still holds the seed)")
                                                   : (f->Completed ? "the fence arrived" : "no fence"));
}

static int Fence(int argc, WCHAR **argv)
{
    BC250_ESCAPE_FENCE f;
    NTSTATUS status;
    int i;

    memset(&f, 0, sizeof(f));
    f.Magic = BC250_ESCAPE_MAGIC;
    f.Command = BC250_ESCAPE_RUN_FENCE;
    f.Count = 1;
    f.Interrupt = 1;
    if (!_wcsicmp(argv[2], L"gfx")) f.Ring = BC250_FENCE_RING_GFX;
    else if (!_wcsicmp(argv[2], L"kiq")) f.Ring = BC250_FENCE_RING_KIQ;
    else if ((argv[2][0] == L'c' || argv[2][0] == L'C') && argv[2][1] >= L'0' && argv[2][1] <= L'7' && argv[2][2] == 0)
        f.Ring = BC250_FENCE_RING_COMPUTE0 + (unsigned long)(argv[2][1] - L'0');
    else if ((argv[2][0] == L's' || argv[2][0] == L'S') && (argv[2][1] == L'0' || argv[2][1] == L'1') && argv[2][2] == 0)
        f.Ring = BC250_FENCE_RING_SDMA0 + (unsigned long)(argv[2][1] - L'0');
    else { fprintf(stderr, "fence gfx|c0..c7|kiq|s0|s1 [count] [noint|test|dispatch|ib], not %ls\n", argv[2]); return 2; }
    for (i = 3; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"noint")) f.Interrupt = BC250_FENCE_MODE_VALUE;
        else if (!_wcsicmp(argv[i], L"test")) f.Interrupt = BC250_FENCE_MODE_RING_TEST;
        else if (!_wcsicmp(argv[i], L"dispatch")) f.Interrupt = BC250_FENCE_MODE_DISPATCH;
        else if (!_wcsicmp(argv[i], L"ib")) f.Interrupt = BC250_FENCE_MODE_IB;
        else f.Count = wcstoul(argv[i], NULL, 0);
    }

    if (SendEscape(BC250_DEFAULT_HWID, &f, sizeof(f), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (f.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (f.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no fence command\n"); return 3; }

    FencePrint(&f, argv[2]);
    return f.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- ib: one indirect buffer through the miniport's own submission path (ADR 0008 stage C) ------------------------------
//
// ib <vmid> <root phys hex> <gpu va hex> <dwords>: GfxSubmitIb() with those arguments, then GfxFenceArrived() polled in
// the driver for up to half a second. The VMID's page directory root is programmed first if it differs from the one that
// VMID was last given; a root of 0 leaves it alone, which is what VMID 0 (the GART aperture) always does. Unlike
// `fence gfx 1 ib` this goes through the gate wddm.c will go through, so it needs EnableGpuSubmit and a bring-up that
// reached stage 8, and it takes the one submission slot: a second call is refused until the first fence arrives.

static int Ib(WCHAR **argv)
{
    BC250_ESCAPE_FENCE f;
    NTSTATUS status;

    memset(&f, 0, sizeof(f));
    f.Magic = BC250_ESCAPE_MAGIC;
    f.Command = BC250_ESCAPE_RUN_FENCE;
    f.Ring = BC250_FENCE_RING_GFX;
    f.Count = 1;
    f.Interrupt = BC250_FENCE_MODE_IB_AT;
    f.Vmid = wcstoul(argv[2], NULL, 0);
    f.RootPhysical = _wcstoui64(argv[3], NULL, 16);
    f.IbAddress = _wcstoui64(argv[4], NULL, 16);
    f.Dwords = wcstoul(argv[5], NULL, 0);
    if (f.Vmid > 15 || f.Dwords == 0 || f.Dwords > BC250_FENCE_IB_MAX_DWORDS)
    {
        fprintf(stderr, "ib <vmid 0..15> <root phys hex> <gpu va hex> <dwords 1..%u>\n", BC250_FENCE_IB_MAX_DWORDS);
        return 2;
    }

    if (SendEscape(BC250_DEFAULT_HWID, &f, sizeof(f), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (f.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (f.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no fence command\n"); return 3; }

    FencePrint(&f, L"gfx");
    return f.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- dcn: a read-only dump of the DCN registers (BC250_ESCAPE_RUN_DCN, ADR 0011 point 3) --------------------------------
//
// "R <name> <offset hex> <value hex>" per register (all 75 of gen_regs.py's DCN_REGISTERS), then a decoded
// summary for HUBP0/OTG0 against the Linux reference (evidence/linux/2026-09-22-E21-linux-reference-4/dmupre.txt),
// so a lab run compares at a glance. No write of any kind; needs BAR5 mapped (EnableMmio), no other gate.

static int Dcn(void)
{
    static BC250_ESCAPE_DCN d;
    NTSTATUS status;

    memset(&d, 0, sizeof(d));
    d.Magic = BC250_ESCAPE_MAGIC;
    d.Command = BC250_ESCAPE_RUN_DCN;

    if (SendEscape(BC250_DEFAULT_HWID, &d, sizeof(d), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (d.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (d.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no DCN command\n"); return 3; }

    printf("dcn: %s, NTSTATUS 0x%08lX %s, %lu registers\n", d.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED",
           d.NtStatus, StatusName((NTSTATUS)d.NtStatus), d.RegCount);
    printf("gates        mmio %s\n", (d.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed (EnableMmio is off)");
    if (d.FaultOffset)
        printf("fault        0x%05lX was refused by the driver's own table; the dump stopped short there\n", d.FaultOffset);
    for (unsigned long i = 0; i < d.RegCount && i < BC250_DCN_REG_COUNT; i++)
        printf("R %-42s 0x%05lX %08lX\n", d.Regs[i].Name, d.Regs[i].Offset, d.Regs[i].Value);
    if (d.Status != BC250_ESCAPE_STATUS_DONE) return 3;

    printf("decoded (this run / Linux reference E21 run 4, dmupre.txt):\n");
    printf("hubp0        address 0x%016llX / 0x0000000270000000\n", d.Hubp0Address);
    printf("             pitch %lu / 1919\n", d.Hubp0Pitch);
    printf("             DCHUBP_CNTL 0x%08lX / 0x000F1002\n", d.Hubp0Cntl);
    printf("otg0         OTG_CONTROL 0x%08lX / 0x80011311, master enable %s / ENABLED\n", d.Otg0Control,
           d.Otg0MasterEnable ? "ENABLED" : "off");
    printf("             h_total %lu / 2079, v_total %lu / 1234\n", d.Otg0HTotal, d.Otg0VTotal);
    printf("             vblank interrupt enable (GLOBAL_SYNC_STATUS bit 12) %s (no Linux reference for this bit alone)\n",
           d.Otg0VblankIntEnabled ? "on" : "off");
    printf("             vblank event occurred (GLOBAL_SYNC_STATUS bit 14) %s (docs/design/vsync-interrupt-route.md: latches\n"
           "             regardless of the enable bit above - a lone way to tell \"never fires\" from \"fires, never arrives\")\n",
           d.Otg0VupdateEventOccurred ? "SET" : "clear");
    printf("             vupdate int_status (GLOBAL_SYNC_STATUS bit 15) %s (empirical, not read by amdgpu's own source:\n"
           "             tracked the enable bit in the only cross-check on record, M92 vs M103 - \"qualified to interrupt\")\n",
           d.Otg0VupdateIntStatus ? "SET" : "clear");
    printf("             master update lock %s, flip pending %s, vupdate keepout %s (all should read off/clear/off outside a flip;\n"
           "             no Linux reference, these hold only mid-sequence)\n",
           d.Otg0MasterUpdateLocked ? "LOCKED" : "unlocked", d.Hubp0FlipPending ? "PENDING" : "clear",
           d.Otg0VupdateKeepoutEn ? "ON" : "off");
    return 0;
}

// ---- dcnflip: one gated flip on HUBP0/OTG0 (BC250_ESCAPE_RUN_DCNFLIP, 0.7.20, ADR 0011 point 3 step 2) ------------------
//
// "bc250kmd_cli dcnflip <physical hex> [fill <argb hex>]" or "bc250kmd_cli dcnflip restore". Needs EnableMmio and
// EnableDcnWrite; fill needs EnableVramWrite as well and is refused for the firmware's own address.

static void PrintDcnFlip(const BC250_ESCAPE_DCNFLIP *d)
{
    printf("dcnflip: %s, NTSTATUS 0x%08lX %s\n", d->Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED",
           d->NtStatus, StatusName((NTSTATUS)d->NtStatus));
    printf("gates        mmio %s, dcn writes %s, vram %s, vram writes %s\n",
           (d->Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed",
           (d->Flags & BC250_ESCAPE_FLAG_DCN_WRITE) ? "open" : "closed (EnableDcnWrite is off)",
           (d->Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed",
           (d->Flags & BC250_ESCAPE_FLAG_VRAM_WRITE) ? "on" : "off (needed for fill)");
    if (d->Reason[0]) printf("reason       %s\n", d->Reason);
    if (d->Status != BC250_ESCAPE_STATUS_DONE) return;
    printf("firmware     0x%016llX\n", d->FirmwareAddress);
    printf("address      0x%016llX -> 0x%016llX\n", d->AddressBefore, d->AddressAfter);
    printf("inuse        0x%08lX -> 0x%08lX\n", d->InUseBefore, d->InUseAfter);
    printf("frame count  %lu -> %lu\n", d->FrameCountBefore, d->FrameCountAfter);
    printf("flip pending %s after %lu us\n", d->FlipPendingCleared ? "cleared" : "STILL SET", d->WaitUs);
    printf("DCHUBP_CNTL  0x%08lX, underflow 0x%08lX%s\n", d->DchubpCntl, d->Underflow, d->Underflow ? " (SET)" : "");
}

static int DcnFlip(const WCHAR *physText, const WCHAR *fillWord, const WCHAR *fillText, int restore)
{
    static BC250_ESCAPE_DCNFLIP d;
    NTSTATUS status;
    WCHAR *end;

    memset(&d, 0, sizeof(d));
    d.Magic = BC250_ESCAPE_MAGIC;
    d.Command = BC250_ESCAPE_RUN_DCNFLIP;
    if (restore) {
        d.Restore = 1;
    } else {
        d.Physical = _wcstoui64(physText, &end, 16);
        if (*end) { fprintf(stderr, "physical address %ls is not a hex number\n", physText); return 2; }
        if (fillWord) {
            if (_wcsicmp(fillWord, L"fill")) { fprintf(stderr, "unknown option %ls (expected fill)\n", fillWord); return 2; }
            if (!fillText) { fprintf(stderr, "fill needs an ARGB hex value\n"); return 2; }
            d.Fill = 1;
            d.FillColor = wcstoul(fillText, &end, 16);
            if (*end) { fprintf(stderr, "fill colour %ls is not a hex number\n", fillText); return 2; }
        }
    }

    if (SendEscape(BC250_DEFAULT_HWID, &d, sizeof(d), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (d.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (d.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no DCN flip command\n"); return 3; }
    PrintDcnFlip(&d);
    return d.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- sdmacopy: the SDMA copy/fill positive control (BC250_ESCAPE_RUN_SDMACOPY, ADR 0013) --------------------------------
//
// "bc250kmd_cli sdmacopy [bytes]": one SDMA constant fill and one linear copy on SDMA0, read back and compared by
// the CPU, that never touches the WDDM table. Needs the same gates as `fence s0 ... test` (EnableGfx, a bring-up
// that has reached stage 7) plus EnableVramWrite. bytes defaults to 4096, up to BC250_SDMACOPY_MAX_BYTES (64 KiB).

static int SdmaCopy(const WCHAR *bytesText, int indirect)
{
    static BC250_ESCAPE_SDMACOPY s;
    NTSTATUS status;
    WCHAR *end;

    memset(&s, 0, sizeof(s));
    s.Magic = BC250_ESCAPE_MAGIC;
    s.Command = indirect ? BC250_ESCAPE_RUN_SDMAIB : BC250_ESCAPE_RUN_SDMACOPY;
    if (bytesText != NULL) {
        s.Bytes = wcstoul(bytesText, &end, 0);
        if (*end || s.Bytes == 0 || s.Bytes > BC250_SDMACOPY_MAX_BYTES) {
            fprintf(stderr, "sdmacopy [bytes 1..%u], not %ls\n", BC250_SDMACOPY_MAX_BYTES, bytesText);
            return 2;
        }
    }

    if (SendEscape(BC250_DEFAULT_HWID, &s, sizeof(s), &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (s.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (s.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no sdmacopy command\n"); return 3; }

    printf("%s %lu bytes: %s, NTSTATUS 0x%08lX %s, result %ld\n", indirect ? "sdmaib VMID0" : "sdmacopy", s.Bytes != 0 ? s.Bytes : BC250_SDMACOPY_DEFAULT_BYTES,
           s.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", s.NtStatus, StatusName((NTSTATUS)s.NtStatus), s.Result);
    printf("gates        mmio %s, vram %s, vram writes %s, gart %s, psp %s, gfx %s\n",
           (s.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed",
           (s.Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed",
           (s.Flags & BC250_ESCAPE_FLAG_VRAM_WRITE) ? "on" : "off (EnableVramWrite is needed)",
           (s.Flags & BC250_ESCAPE_FLAG_GART) ? "open" : "closed", (s.Flags & BC250_ESCAPE_FLAG_PSP) ? "open" : "closed",
           (s.Flags & BC250_ESCAPE_FLAG_GFX) ? "open" : "closed");
    if (s.FaultOffset) printf("fault        0x%08lX was refused by the driver's table; the sequence stopped there\n", s.FaultOffset);
    printf("regions      src 0x%016llX, dst 0x%016llX (VRAM, MC addresses)\n", s.SrcMc, s.DstMc);
    printf("fence        emitted 0x%lX, slot holds 0x%lX\n", s.LastSeq, s.LastValue);
    printf("compare      %lu bytes, %s", s.BytesCompared, s.Matched ? "MATCHED (every byte read back 0xA5)\n" : "MISMATCH\n");
    if (!s.Matched)
        printf("             first wrong byte at offset 0x%lX: got 0x%02lX, want 0x%02lX\n", s.FirstMismatchOffset,
               s.FirstMismatchGot, s.FirstMismatchWant);
    printf("time         %lu us, seed to read-back\n", s.Microseconds);
    return s.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}

// ---- fbdump: assemble the scanned-out surface into a BMP (BC250_ESCAPE_RUN_FBDUMP, 2026-09-22) ---------------------------
//
// "bc250kmd_cli fbdump <file.bmp>": BC250_FBDUMP_MAX_ROWS rows per escape, until the whole surface is in a
// buffer of our own, then one 32-bit BI_RGB BMP written bottom-up as the format requires. Only a vertical flip
// is needed: A8R8G8B8's byte order in memory (B, G, R, A on this little-endian target) already matches BMP's,
// so there is no channel swap to do, only the row order.

#pragma pack(push, 1)
typedef struct { unsigned short Type; unsigned long Size, Reserved, OffBits; } BC250_BMP_FILE_HEADER;
typedef struct {
    unsigned long HeaderSize, Width; long Height; unsigned short Planes, BitCount;
    unsigned long Compression, SizeImage; long XPelsPerMeter, YPelsPerMeter;
    unsigned long ClrUsed, ClrImportant;
} BC250_BMP_INFO_HEADER;
#pragma pack(pop)

static int Fbdump(const WCHAR *path)
{
    static BC250_ESCAPE_FBDUMP f;
    unsigned char *frame = NULL;
    unsigned long width = 0, height = 0, pitch = 0, row;
    NTSTATUS status;
    FILE *out;

    memset(&f, 0, sizeof(f));
    f.Magic = BC250_ESCAPE_MAGIC;
    f.Command = BC250_ESCAPE_RUN_FBDUMP;
    f.Hubp = 0;
    f.FirstRow = 0;
    f.RowCount = BC250_FBDUMP_MAX_ROWS;

    for (;;) {
        if (SendEscape(BC250_DEFAULT_HWID, &f, sizeof(f), &status)) { free(frame); return 1; }
        if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); free(frame); return 1; }
        if (f.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); free(frame); return 3; }
        if (f.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) { printf("refused: this driver build has no fbdump command\n"); free(frame); return 3; }
        if (f.Status != BC250_ESCAPE_STATUS_DONE) {
            printf("fbdump refused at row %lu: NTSTATUS 0x%08lX %s%s%s\n", f.FirstRow, f.NtStatus, StatusName((NTSTATUS)f.NtStatus),
                   f.Reason[0] ? ": " : "", f.Reason);
            printf("gates        mmio %s, vram %s\n", (f.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "closed (EnableMmio is off)",
                   (f.Flags & BC250_ESCAPE_FLAG_VRAM) ? "identified" : "closed");
            free(frame);
            return 3;
        }

        if (frame == NULL) {
            width = f.Width; height = f.Height; pitch = f.Pitch;
            frame = (unsigned char *)malloc((size_t)height * pitch);
            if (frame == NULL) { fprintf(stderr, "out of memory for %lux%lu\n", width, height); return 1; }
        } else if (f.Width != width || f.Height != height || f.Pitch != pitch) {
            fprintf(stderr, "geometry changed mid-dump (%lux%lu pitch %lu -> %lux%lu pitch %lu): a mode change during the dump\n",
                    width, height, pitch, f.Width, f.Height, f.Pitch);
            free(frame);
            return 1;
        }
        memcpy(frame + (size_t)f.FirstRow * pitch, f.Pixels, (size_t)f.RowCount * pitch);
        printf("fbdump       rows %lu..%lu of %lu, address 0x%llX\n", f.FirstRow, f.FirstRow + f.RowCount - 1, height, f.Address);

        row = f.FirstRow + f.RowCount;
        if (row >= height) break;
        f.FirstRow = row;
        f.RowCount = (height - row < BC250_FBDUMP_MAX_ROWS) ? height - row : BC250_FBDUMP_MAX_ROWS;
    }

    out = _wfopen(path, L"wb");
    if (out == NULL) { fprintf(stderr, "cannot create %ls\n", path); free(frame); return 1; }
    {
        BC250_BMP_FILE_HEADER fh = { 0x4D42, 0, 0, sizeof(BC250_BMP_FILE_HEADER) + sizeof(BC250_BMP_INFO_HEADER) };
        BC250_BMP_INFO_HEADER ih = { sizeof(BC250_BMP_INFO_HEADER), width, (long)height, 1, 32, 0, pitch * height, 0, 0, 0, 0 };
        long y;

        fh.Size = fh.OffBits + ih.SizeImage;
        fwrite(&fh, sizeof(fh), 1, out);
        fwrite(&ih, sizeof(ih), 1, out);
        for (y = (long)height - 1; y >= 0; y--) fwrite(frame + (size_t)y * pitch, pitch, 1, out);
    }
    fclose(out);
    printf("wrote %ls: %lux%lu, %lu bytes/row, %lu bytes total\n", path, width, height, pitch, (unsigned long)((size_t)height * pitch));
    free(frame);
    return 0;
}

// ---- stages: the registry ------------------------------------------------------------------------------------

static int ReadDword(const WCHAR *key, const WCHAR *name, DWORD *value)
{
    DWORD size = sizeof(*value), type = 0;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, &type, value, &size) == ERROR_SUCCESS;
}

static int Stages(void)
{
    HKEY key;
    DWORD stage = 0, starts = 0;
    WCHAR history[1024];
    DWORD size = sizeof(history), type = 0;
    LSTATUS s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, BC250_SERVICE_KEY, 0, KEY_READ, &key);

    if (s == ERROR_FILE_NOT_FOUND) {
        printf("bc250kmd is not installed (no HKLM\\%ls)\n", BC250_SERVICE_KEY);
        return 2;
    }
    if (s != ERROR_SUCCESS) {
        fprintf(stderr, "cannot open HKLM\\%ls, error %lu\n", BC250_SERVICE_KEY, (unsigned long)s);
        return 1;
    }
    RegCloseKey(key);

    printf("service      installed (HKLM\\%ls)\n", BC250_SERVICE_KEY);
    if (ReadDword(BC250_PARAMETERS, L"LastStage", &stage))
        printf("LastStage    %lu %s\n", stage, StageName(stage));
    else
        printf("LastStage    -  (the driver has not written one yet)\n");

    if (RegGetValueW(HKEY_LOCAL_MACHINE, BC250_PARAMETERS, L"StageHistory", RRF_RT_REG_SZ, &type, history, &size) == ERROR_SUCCESS)
        printf("StageHistory %ls\n", history);
    else
        printf("StageHistory -\n");

    if (ReadDword(BC250_PARAMETERS, L"UnconfirmedStarts", &starts)) {
        printf("starts       %lu unconfirmed%s\n", starts,
               starts >= 2 ? "  (the guard refuses to start: run 'bc250kmd_cli confirm')" : "");
        return starts >= 2 ? 3 : 0;
    }
    printf("starts       -\n");
    return 0;
}

static int Confirm(void)
{
    HKEY key;
    DWORD zero = 0, disposition = 0;
    LSTATUS s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, BC250_SERVICE_KEY, 0, KEY_READ, &key);

    // Never bring the service key into being: an empty Services\bc250kmd would be a puzzle for whoever looks
    // at the machine next, and this tool is run on machines where the driver is deliberately not installed.
    if (s == ERROR_FILE_NOT_FOUND) {
        fprintf(stderr, "bc250kmd is not installed (no HKLM\\%ls), nothing to confirm\n", BC250_SERVICE_KEY);
        return 2;
    }
    if (s != ERROR_SUCCESS) {
        fprintf(stderr, "cannot open HKLM\\%ls, error %lu%s\n", BC250_SERVICE_KEY, (unsigned long)s,
                s == ERROR_ACCESS_DENIED ? " (is this an elevated prompt?)" : "");
        return 1;
    }
    RegCloseKey(key);

    s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, BC250_PARAMETERS, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &key, &disposition);
    if (s != ERROR_SUCCESS) {
        fprintf(stderr, "cannot open HKLM\\%ls, error %lu%s\n", BC250_PARAMETERS, (unsigned long)s,
                s == ERROR_ACCESS_DENIED ? " (is this an elevated prompt?)" : "");
        return 1;
    }
    s = RegSetValueExW(key, L"UnconfirmedStarts", 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
    if (s != ERROR_SUCCESS) {
        RegCloseKey(key);
        fprintf(stderr, "cannot write UnconfirmedStarts, error %lu\n", (unsigned long)s);
        return 1;
    }
    // RegFlushKey requires KEY_QUERY_VALUE and returns the persistence status.
    // Closing the handle or rereading zero is not proof it reached the disk.
    s = RegFlushKey(key);
    RegCloseKey(key);
    if (s != ERROR_SUCCESS) {
        fprintf(stderr, "cannot persist UnconfirmedStarts = 0, flush error %lu\n", (unsigned long)s);
        return 1;
    }
    printf("UnconfirmedStarts = 0 (flushed to disk)\n");
    return 0;
}

// ---- log: the driver's own log ring (BC250_ESCAPE_GET_LOG) -------------------------------------------------------
//
// The lab machine is headless over SSH: no kernel debugger, no DebugView, so the debug print stream GuardLog
// writes to reaches nobody. Since 0.7.1 every line also goes into a ring inside the driver image, and this pages
// through it, 64 lines per escape, printing "sequence seconds.milliseconds text".
//
// Two things to know when reading it. The ring lives in the driver image, so it starts empty at every driver load
// and dies with one: read it BEFORE closing a gate that needs a reload. And a sequence number that starts at 0
// again is itself the proof that the driver really unloaded.
//
// "log summary" asks the WDDM table to write its call counters into the ring first, which is what stage A is run
// for; with the gate closed that is one line saying the table is not running.

static int Log(const WCHAR *fromText, int summary)
{
    static BC250_ESCAPE_LOG log;        // 10 KB: a static, not a frame this tool has no reason to grow
    unsigned long from = 0, printed = 0;
    int first = 1;
    NTSTATUS status;
    WCHAR *end;

    if (fromText != NULL) {
        from = wcstoul(fromText, &end, 10);
        // wcstoul takes "-1" and returns 0xFFFFFFFF, which is BC250_LOG_FROM_SUMMARY: a sentinel is not a number to type.
        if (*end || fromText[0] == L'-' || from == BC250_LOG_FROM_SUMMARY) {
            fprintf(stderr, "log [from], where from is a decimal sequence number, not %ls\n", fromText);
            return 2;
        }
    }
    for (;;) {
        memset(&log, 0, sizeof(log));
        log.Magic = BC250_ESCAPE_MAGIC;
        log.Command = (summary && first) ? BC250_ESCAPE_LOG_SUMMARY : BC250_ESCAPE_GET_LOG;
        log.From = from;
        if (SendEscape(BC250_DEFAULT_HWID, &log, sizeof(log), &status)) return 1;
        if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
        if (log.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
        if (log.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) {
            printf("refused: this driver build has no log command (0.7.0 or older)\n");
            return 3;
        }
        if (log.Status != BC250_ESCAPE_STATUS_DONE) {
            printf("refused: driver status %lu, NTSTATUS 0x%08lX %s\n", log.Status, log.NtStatus,
                   StatusName((NTSTATUS)log.NtStatus));
            return 3;
        }
        if (first)
            printf("log          %lu lines since this driver load, %lu lost to the wrap, %lu dropped above "
                   "DISPATCH_LEVEL; ring %lu lines of which the first %lu are kept; table %s\n",
                   log.Total, log.Lost, log.Above, log.RingLines, log.HeadLines,
                   (log.Flags & BC250_ESCAPE_FLAG_FULL_WDDM) ? "FULL WDDM (the gate was open at DriverEntry)" :
                                                               "display-only");
        first = 0;
        for (unsigned long i = 0; i < log.Returned && i < BC250_LOG_MAX_LINES; i++) {
            log.Lines[i].Text[BC250_LOG_TEXT - 1] = 0;      // the driver terminates it; printing does not rely on that
            printf("%6lu %6lu.%03lu %s\n", log.Lines[i].Sequence, log.Lines[i].Milliseconds / 1000,
                   log.Lines[i].Milliseconds % 1000, log.Lines[i].Text);
            printed++;
        }
        if (log.Returned == 0 || log.Next <= from) break;   // the end, or a driver that is not moving on
        from = log.Next;
        // A driver that keeps logging while we read would keep us here: the ring is 1024 lines, so anything past
        // a few times that is a live stream rather than a trail, and whoever wants more can ask again.
        if (printed > 4 * log.RingLines) { printf("             stopped at %lu lines; ask again from %lu\n", printed, from); break; }
    }
    printf("             %lu lines printed\n", printed);
    return 0;
}

// ---- ---------------------------------------------------------------------------------------------------------

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bc250kmd_cli info [hardware-id] | list | stages | confirm\n"
                        "       bc250kmd_cli health read | health confirm <generation> <epoch>\n"
                        "       bc250kmd_cli clock read | clock set <MHz> <mV>\n"
                        "       bc250kmd_cli read <hex offset> | write <hex offset> <hex value>\n"
                        "       bc250kmd_cli memory | vread <phys|bar0> <hex offset> | vwrite <phys|bar0> <hex offset> <hex value>\n"
                        "       bc250kmd_cli vcompare <hex offset> <count>\n"
                        "       bc250kmd_cli vtable <hex offset>          (one page table page, nonzero entries)\n"
                        "       bc250kmd_cli gart plan | enable | restore\n"
                        "       bc250kmd_cli psp plan | load | unload\n"
                        "       bc250kmd_cli gfx plan <stage> | run <stage> | fini | state\n"
                        "       bc250kmd_cli ih plan | init | fini | state\n"
                        "       bc250kmd_cli dcn                          (read-only dump of the DCN registers, ADR 0011)\n"
                        "       bc250kmd_cli dcnflip <phys hex> [fill <argb hex>] | dcnflip restore\n"
                        "       bc250kmd_cli fence <ring> <count> [noint|test|dispatch|ib]\n"
                        "       bc250kmd_cli ib <vmid> <root phys hex> <gpu va hex> <dwords>\n"
                        "       bc250kmd_cli sdmaib [bytes]               (VMID0 indirect SDMA copy/fill control)\n"
                        "       bc250kmd_cli sdmacopy [bytes]             (SDMA copy/fill positive control, ADR 0013)\n"
                        "       bc250kmd_cli fbdump <file.bmp>            (the scanned-out surface, HUBP0, as a BMP)\n"
                        "       bc250kmd_cli log [from] | log summary [from]\n"
                        "       default hardware id: %ls\n", BC250_DEFAULT_HWID);
        return 2;
    }
    if (!_wcsicmp(argv[1], L"health")) return StartHealth(argc,argv);
    if (!_wcsicmp(argv[1], L"clock")) return Clock(argc,argv);
    if (!_wcsicmp(argv[1], L"info")) return Info(argc > 2 ? argv[2] : BC250_DEFAULT_HWID);
    if (!_wcsicmp(argv[1], L"list")) return ListAdapters();
    if (!_wcsicmp(argv[1], L"stages")) return Stages();
    if (!_wcsicmp(argv[1], L"confirm")) return Confirm();
    if (!_wcsicmp(argv[1], L"read") && argc == 3) return Register(0, argv[2], NULL);
    if (!_wcsicmp(argv[1], L"write") && argc == 4) return Register(1, argv[2], argv[3]);
    if (!_wcsicmp(argv[1], L"memory")) return Memory();
    if (!_wcsicmp(argv[1], L"vread") && argc == 4) return VramWord(0, argv[2], argv[3], NULL);
    if (!_wcsicmp(argv[1], L"vwrite") && argc == 5) return VramWord(1, argv[2], argv[3], argv[4]);
    if (!_wcsicmp(argv[1], L"vcompare") && argc == 4) return VramCompare(argv[2], argv[3]);
    if (!_wcsicmp(argv[1], L"vtable") && argc == 3) return VramTable(argv[2]);
    if (!_wcsicmp(argv[1], L"gart") && argc == 3) return Gart(argv[2]);
    if (!_wcsicmp(argv[1], L"psp") && argc == 3) return Psp(argv[2]);
    if (!_wcsicmp(argv[1], L"gfx") && (argc == 3 || argc == 4)) return Gfx(argv[2], argc == 4 ? argv[3] : NULL);
    if (!_wcsicmp(argv[1], L"ih") && argc == 3) return Ih(argv[2]);
    if (!_wcsicmp(argv[1], L"dcn")) return Dcn();
    if (!_wcsicmp(argv[1], L"dcnflip") && argc == 3 && !_wcsicmp(argv[2], L"restore")) return DcnFlip(NULL, NULL, NULL, 1);
    if (!_wcsicmp(argv[1], L"dcnflip") && argc == 3) return DcnFlip(argv[2], NULL, NULL, 0);
    if (!_wcsicmp(argv[1], L"dcnflip") && argc == 5) return DcnFlip(argv[2], argv[3], argv[4], 0);
    if (!_wcsicmp(argv[1], L"fence") && argc >= 3 && argc <= 5) return Fence(argc, argv);
    if (!_wcsicmp(argv[1], L"ib") && argc == 6) return Ib(argv);
    if (!_wcsicmp(argv[1], L"sdmacopy") && argc <= 3) return SdmaCopy(argc == 3 ? argv[2] : NULL, 0);
    if (!_wcsicmp(argv[1], L"sdmaib") && argc <= 3) return SdmaCopy(argc == 3 ? argv[2] : NULL, 1);
    if (!_wcsicmp(argv[1], L"fbdump") && argc == 3) return Fbdump(argv[2]);
    if (!_wcsicmp(argv[1], L"log") && argc <= 4) {
        if (argc >= 3 && !_wcsicmp(argv[2], L"summary")) return Log(argc == 4 ? argv[3] : NULL, 1);
        if (argc <= 3) return Log(argc == 3 ? argv[2] : NULL, 0);
    }
    fprintf(stderr, "unknown command %ls\n", argv[1]);
    return 2;
}
