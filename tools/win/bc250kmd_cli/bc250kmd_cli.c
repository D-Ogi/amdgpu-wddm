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
//   bc250kmd_cli health read|confirm  the cached start-health witness, and its checked confirmation
//   bc250kmd_cli clock read|set       one KMD clock sample, or a complete operating-point transaction
//   bc250kmd_cli telemetry | vram     what the monitor's GPU line shows (bc250control.dll exports the same reads)
//   bc250kmd_cli dpm [n [ms]]       the DPM governor's telemetry, n samples; dpm confirm clears a pending DPM start
//   bc250kmd_cli dpm tune|floor ...   the governor's thresholds and a runtime floor, until the next device start (0.7.185)
//   bc250kmd_cli dpm curve ...        the operator's V/F curve and its trial (0.7.213, docs/design/tuner.md)
//   bc250kmd_cli cpu ...              the CPU clock limit, undervolt, temperature cap and core mask (0.7.213)
//                                     the header also names the idle state: its point, window and counters (0.7.207)
//   bc250kmd_cli interop              the GPU DWM interop switches this start runs with, and why
//   bc250kmd_cli dpaudio [state]      the DP audio check table (step 0 reads) and the record of the last start
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
#include <tlhelp32.h>       // `budget <image>`: a process by its image name

#include "../../../driver/kmd/bc250kmd_escape.h"     // shared with the driver, never copied
#include "../../../driver/kmd/regs.generated.h"      // `read <name>`: the named READ_REG offsets, from gen_regs.py
#include "../../../third_party/linux-amdgpu/dcn_2_0_1_sh_mask.h"  // `dpaudio`: field masks to decode the reply

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

// Finds the adapter whose hardware id matches and opens it: 0 with *handle set, or 1 after reporting why not.
static int OpenAdapterById(const WCHAR *wantedId, D3DKMT_HANDLE *handle, NTSTATUS *result)
{
    BC250_ADAPTER adapters[16];
    int count = FindAdapters(adapters, 16);
    int chosen = -1;
    D3DKMT_OPENADAPTERFROMDEVICENAME open = { 0 };

    for (int i = 0; i < count && chosen < 0; i++)
        if (MatchesHardwareId(adapters[i].HardwareId, wantedId)) chosen = i;
    if (chosen < 0) { fprintf(stderr, "no display adapter with hardware id %ls\n", wantedId); return 1; }

    open.pDeviceName = adapters[chosen].InterfacePath;
    *result = D3DKMTOpenAdapterFromDeviceName(&open);
    if (!NT_SUCCESS(*result)) { PrintStatus("D3DKMTOpenAdapterFromDeviceName", *result); return 1; }
    *handle = open.hAdapter;
    return 0;
}

static NTSTATUS EscapeOn(D3DKMT_HANDLE adapter, void *data, unsigned size, int softwareOnly)
{
    D3DKMT_ESCAPE escape = { 0 };

    escape.hAdapter = adapter;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    if (softwareOnly) escape.Flags.NoAdapterSynchronization = 1;
    else escape.Flags.HardwareAccess = 1;   // dxgkrnl then serializes the call with the rest of the adapter's work
    escape.pPrivateDriverData = data;
    escape.PrivateDriverDataSize = size;
    return D3DKMTEscape(&escape);
}

static void CloseAdapterHandle(D3DKMT_HANDLE adapter)
{
    D3DKMT_CLOSEADAPTER close = { 0 };

    close.hAdapter = adapter;
    D3DKMTCloseAdapter(&close);
}

// With softwareOnly the escape carries NoAdapterSynchronization and nothing else: the typed snapshots (DPM) that
// the driver answers without idling the adapter refuse any other flag combination.
static int SendEscapeFlags(const WCHAR *wantedId, void *data, unsigned size, int softwareOnly, NTSTATUS *result)
{
    D3DKMT_HANDLE adapter = 0;

    if (OpenAdapterById(wantedId, &adapter, result)) return 1;
    *result = EscapeOn(adapter, data, size, softwareOnly);
    CloseAdapterHandle(adapter);
    return 0;
}

// ---- reads of the log ring and the paging journal without adapter synchronization (0.7.184.1) --------------------
//
// The commands the driver answers with NoAdapterSynchronization alone from 0.7.184.1 on (display.c
// SoftwareReadEscape; test_escape_flags.py keeps the two lists equal). With HardwareAccess dxgkrnl takes the adapter
// lock for the escape, and a lab profile of a Witcher 3 session (2026-10-01) put the game's main thread in WrResource
// waits readied by this tool, 1.2-1.3 ms per frame, from the samplers' `log N`, `log summary` and `journal` reads.
// LOG_SUMMARY is not on the list: the driver walks state a stop frees and reads display registers for it, and
// refuses it without HardwareAccess (display.c LogEscape), so its first page keeps the old flags.
static int SoftwareRead(unsigned long command)
{
    switch (command) {
    case BC250_ESCAPE_GET_LOG:
    case BC250_ESCAPE_GET_PAGING_JOURNAL:
        return 1;
    default:
        return 0;
    }
}

// A driver up to 0.7.183.1 refuses NoAdapterSynchronization for these commands: STATUS_DEVICE_NOT_READY, with
// Status REFUSED and nothing else written, Version included. (0.7.184.1 writes Version before any refusal of its
// own.) The first refused software read therefore sends the same request again with HardwareAccess on the same
// handle, and every later read of this process does so at once; the held adapter of `journal follow` forgets it
// when it reopens (a reloaded driver may be a newer one).
static int g_ReadsHard;                     // a software read was refused: this driver wants HardwareAccess
static unsigned long g_SoftReads, g_HardReads;

typedef char BC250_CLI_READ_FITS[(sizeof(BC250_ESCAPE_PAGING_JOURNAL) <= sizeof(BC250_ESCAPE_LOG)) ? 1 : -1];
// Status and Version sit at the same offsets in both replies (the common head of the escape structures).
typedef char BC250_CLI_READ_HEAD[(FIELD_OFFSET(BC250_ESCAPE_LOG, Status) == FIELD_OFFSET(BC250_ESCAPE_PAGING_JOURNAL, Status) &&
                                  FIELD_OFFSET(BC250_ESCAPE_LOG, Version) == FIELD_OFFSET(BC250_ESCAPE_PAGING_JOURNAL, Version))
                                 ? 1 : -1];

static NTSTATUS ReadEscapeOn(D3DKMT_HANDLE adapter, unsigned long command, void *data, unsigned size)
{
    static unsigned char request[sizeof(BC250_ESCAPE_LOG)];     // the request as sent, for the fallback
    const BC250_ESCAPE_LOG *reply = (const BC250_ESCAPE_LOG *)data;     // Status and Version only (see above)
    NTSTATUS status;

    if (SoftwareRead(command) && !g_ReadsHard && size <= sizeof(request)) {
        memcpy(request, data, size);
        status = EscapeOn(adapter, data, size, 1);
        if (NT_SUCCESS(status) && !(reply->Status == BC250_ESCAPE_STATUS_REFUSED && reply->Version == 0)) {
            g_SoftReads++;
            return status;
        }
        memcpy(data, request, size);        // whatever the refusal wrote: the same request again
        g_ReadsHard = 1;
    }
    g_HardReads++;
    return EscapeOn(adapter, data, size, 0);
}

// One read with its own adapter open and close, as SendEscape.
static int SendReadEscape(const WCHAR *wantedId, unsigned long command, void *data, unsigned size, NTSTATUS *result)
{
    D3DKMT_HANDLE adapter = 0;

    if (OpenAdapterById(wantedId, &adapter, result)) return 1;
    *result = ReadEscapeOn(adapter, command, data, size);
    CloseAdapterHandle(adapter);
    return 0;
}

// The held adapter of `journal follow` (0.7.183.1): opened at the first read and kept for the whole run, so a
// sampler no longer walks the display device interfaces and opens the adapter once per interval. A failed escape
// closes it (a PnP disable/enable of the adapter, as the GPU DWM ladder does, leaves the handle stale), and the
// next read opens it again. Nothing else in this tool holds an adapter. From 0.7.184.1 its reads go through
// ReadEscapeOn.
static D3DKMT_HANDLE g_HeldAdapter;
static unsigned long g_HeldOpens;

static int SendEscapeHeld(const WCHAR *wantedId, unsigned long command, void *data, unsigned size, NTSTATUS *result)
{
    if (g_HeldAdapter == 0) {
        if (OpenAdapterById(wantedId, &g_HeldAdapter, result)) { g_HeldAdapter = 0; return 1; }
        g_HeldOpens++;
        g_ReadsHard = 0;
    }
    *result = ReadEscapeOn(g_HeldAdapter, command, data, size);
    if (!NT_SUCCESS(*result)) { CloseAdapterHandle(g_HeldAdapter); g_HeldAdapter = 0; }
    return 0;
}

static void ReleaseHeldAdapter(void)
{
    if (g_HeldAdapter != 0) { CloseAdapterHandle(g_HeldAdapter); g_HeldAdapter = 0; }
}

static int SendEscape(const WCHAR *wantedId, void *data, unsigned size, NTSTATUS *result)
{
    return SendEscapeFlags(wantedId, data, size, 0, result);
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
    if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),op!=BC250_CLOCK_OP_SET,&status))return status;
    if(!NT_SUCCESS(status))return status;
    if(data->Status==BC250_ESCAPE_STATUS_UNKNOWN_COMMAND)return (LONG)0xC00000BB;
    if(data->Status!=BC250_ESCAPE_STATUS_DONE || !data->Ready)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    return 0;
}

// READ is an adapter-owned software snapshot, never a hardware-idling diagnostic, and from 0.7.213 CONFIRM is one
// too: it touches that snapshot and the registry, so both go with NoAdapterSynchronization alone. Up to 0.7.212
// CONFIRM carried HardwareAccess, which suspends the GPU scheduler for up to one VSync while the driver flushes a
// registry key - and the installer's logon task retries it every 5.5 s for two minutes. A driver of 0.7.212 or
// older still insists on HardwareAccess, which happens whenever a release defers the device restart, so a refused
// CONFIRM is sent again with the old word (see the retry below) instead of leaving the start unconfirmed.
BC250_CONTROL_API LONG WINAPI Bc250StartHealth(ULONG op,ULONGLONG generation,ULONGLONG epoch,
    BC250_ESCAPE_START_HEALTH* data,ULONG bytes)
{
    static int confirmLegacy;       // a CONFIRM was refused without HardwareAccess: this driver is 0.7.212 or older
    BC250_ESCAPE_START_HEALTH request;
    const int confirm=op==BC250_START_HEALTH_CONFIRM;
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
    request=*data;                  // the request as built, for the retry below
    // A driver up to 0.7.212 demands HardwareAccess for CONFIRM and refuses this word at its gate: Status REFUSED
    // with NtStatus STATUS_INVALID_PARAMETER, and Version written before the refusal (start_health.c). A
    // well-formed request of this release gets that answer from nothing else, so it is the signal to send the same
    // request once more with the old word and to keep sending it that way for the rest of the process - the mirror
    // image of ReadEscapeOn's fallback, which carries a pre-0.7.184.1 driver the other way.
    // The pair is not hypothetical: the installer copies the tools and defers the device restart, so this
    // release's logon task, the overlay's automatic confirmation and the control application's Recovery action all
    // run against the loaded old driver until the next start. Without the retry every one of them is refused for
    // 120 s and start-confirm.ps1 falls back to `bc250kmd_cli confirm`, which writes the boot-loop guard alone:
    // the CU-mode and DPM requests stay unconfirmed and the next start comes up at 24 CU and the floor clock.
    if(confirm && confirmLegacy) {
        if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),0,&status))return status;
    } else {
        if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),1,&status))return status;
        if(confirm && NT_SUCCESS(status) &&
           data->Status==BC250_ESCAPE_STATUS_REFUSED && data->NtStatus==0xC000000Dul) {
            confirmLegacy=1;
            *data=request;
            if(SendEscapeFlags(BC250_DEFAULT_HWID,data,sizeof(*data),0,&status))return status;
        }
    }
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

// ---- telemetry: the monitor's GPU line (tools/win/bc250mon/src/TelemetryProvider.cs) --------------------------
//
// Two read-only sources the monitor polls four times a second, so neither may cost a SetupAPI walk or an SMU
// message per call:
//   BC250_ESCAPE_RUN_DPM READ  the snapshot the KMD's clock governor publishes every 25 ms tick: temperature, the
//                              SMU's clock readback and, from 0.7.177, the GRBM_STATUS busy share. Software state,
//                              NoAdapterSynchronization=1, open to every caller (driver/kmd/dpm.c DpmRequest).
//   D3DKMTQueryStatistics      dxgkrnl's own residency per segment: standard WDDM, whatever KMD is installed.
// The adapter's interface path is looked up once and kept; every call still opens and closes its own adapter
// handle, as the exports above do, so no handle outlives a driver update. A failed open looks the path up again.

#ifndef BC250_ESCAPE_RUN_DPM
// Only for a driver/kmd/bc250kmd_escape.h that predates the DPM escape (KMD 0.7.175 and later); this tree's header
// defines it, so here the block drops out. It is the 0.7.207 definition under the header's own names;
// tools/win/bc250mon/test_telemetry.py compares it with the header of the KMD that ships it.
#define BC250_ESCAPE_RUN_DPM 23u
#define BC250_DPM_ABI 2u
#define BC250_DPM_ABI_1 1u
#define BC250_DPM_ABI1_SIZE 160u
#define BC250_DPM_OP_READ 0u
#define BC250_DPM_FLAG_TEMPERATURE 128u
#define BC250_DPM_FLAG_CLOCK 256u
#define BC250_DPM_FLAG_HW_BUSY 512u
#define BC250_DPM_FLAG_IDLE 1024u
typedef struct _BC250_ESCAPE_DPM {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long Mode, Requested, Reason, Throttle;
    unsigned long MaxMHz, CapMHz, TargetMHz, WantMHz;
    unsigned long CurrentMHz, CurrentMv, ObservedMHz, ObservedVid;
    long TemperatureMc;
    unsigned long BusyPermille, BusyAvgPermille;
    unsigned long Raises, Lowers, ThermalEvents, Errors, Resyncs;
    unsigned long long Ticks;
    unsigned long long BusyTime100ns;
    unsigned long long UptimeMs;
    unsigned long long Generation;
    unsigned long long ExpectedGeneration;
    unsigned long SubmitBusyPermille;
    unsigned long SdmaBusyPermille;
    // ABI 2 from here (BC250_DPM_ABI1_SIZE bytes above). All out.
    unsigned long IdleMHz;
    unsigned long IdleHoldMs, IdleBusyPermille;
    unsigned long IdleEntries, IdleExits, IdleRefusals;
    unsigned long long IdleMs;
} BC250_ESCAPE_DPM; // 192 bytes on Windows, ABI 2 (the first 160 are ABI 1)
#endif

#ifndef BC250_DPM_ABI_3
// Only for a driver/kmd/bc250kmd_escape.h before 0.7.215, which has no RUN_DPM ABI 3; this tree's header defines it,
// so here the block drops out. It is the 0.7.215 definition under the header's own names.
#define BC250_DPM_ABI_3 3u
#define BC250_DPM_ABI2_SIZE 192u
#define BC250_DPM_ABI3_SIZE 248u
#define BC250_DPM_FLAG_POWER 8192u
#define BC250_DPM_METRICS_OFF 0u
#define BC250_DPM_METRICS_WAITING 1u
#define BC250_DPM_METRICS_OK 2u
#define BC250_DPM_METRICS_REFUSED 3u
#define BC250_DPM_METRICS_NO_TABLE 4u
#define BC250_DPM_METRICS_BAD_TABLE 5u
typedef struct _BC250_DPM_METRICS {
    unsigned long MetricsState, MetricsAgeMs, MetricsReads, MetricsFailures;
    unsigned long SocketPowerMw, SocketPowerAvgMw;
    unsigned long GfxPowerMw, SocPowerMw;
    unsigned long GfxMv, SocMv;
    unsigned long GfxMHz;
    unsigned long GfxTemperatureCc, SocTemperatureCc;
    unsigned long ThrottlerStatus;
} BC250_DPM_METRICS; // 56 bytes
typedef struct _BC250_ESCAPE_DPM_EX {
    BC250_ESCAPE_DPM Dpm;
    BC250_DPM_METRICS Metrics;
} BC250_ESCAPE_DPM_EX; // 248 bytes on Windows, ABI 3
#endif

// The monitor's digest of dxgkrnl's segment statistics, not a KMD structure. Memory segments are what Task
// Manager calls dedicated memory; aperture segments (the GART) are summed apart. Segment ids are zero-based.
// tools/win/bc250mon/src/Driver.cs mirrors it; test_telemetry.py keeps the two equal.
#define BC250_VIDEO_MEMORY_SEGMENTS 8u
#define BC250_VIDEO_MEMORY_MAX_SEGMENTS 32u
typedef struct _BC250_VIDEO_MEMORY {
    ULONG Size;                             // sizeof(BC250_VIDEO_MEMORY)
    ULONG Segments;                         // NbSegments; the first BC250_VIDEO_MEMORY_SEGMENTS are itemized
    ULONG ApertureMask;                     // bit i: segment i is an aperture segment
    ULONG LuidLow;
    LONG LuidHigh;
    ULONG Reserved;
    ULONGLONG LocalResident, LocalCommitted, LocalLimit;    // memory segments, summed
    ULONGLONG ApertureResident, ApertureLimit;              // aperture segments, summed
    ULONGLONG DedicatedVideoMemory;         // KMTQAITYPE_GETSEGMENTSIZE, what DXGI reports; 0 when refused
    ULONGLONG Resident[BC250_VIDEO_MEMORY_SEGMENTS];
    ULONGLONG Committed[BC250_VIDEO_MEMORY_SEGMENTS];
    ULONGLONG Limit[BC250_VIDEO_MEMORY_SEGMENTS];
} BC250_VIDEO_MEMORY; // 264 bytes

// What a caller asks the CPU surface for (Bc250Cpu). Size is sizeof(BC250_CPU_REQUEST), so that an older caller
// and a newer DLL recognize each other instead of reading past the buffer. tools/win/amdgpu_wddm_control mirrors
// it in src/Native.cs and its unit tests check the offsets against this text.
typedef struct _BC250_CPU_REQUEST {
    ULONG Size;                             // sizeof(BC250_CPU_REQUEST), 56
    ULONG Op;                               // BC250_CPU_OP_*
    ULONG Given;                            // SET: BC250_CPU_GIVEN_* bits
    ULONG MaxMHz, UvSteps, TempC;           // SET; UvSteps also carries SEARCH_BEGIN's depth
    ULONG TrialMs;                          // SET and SEARCH_BEGIN; 0 is the driver's own window
    ULONG CoreMask;                         // CORES: 119 or 255
    // What the caller knows about the load it runs and the driver cannot see (0.7.211). The driver judges
    // clock stretching only over a sample the caller marks Loaded, and a machine check or a memory checksum
    // error is a sign no kernel reading of this surface carries.
    ULONG WheaEvents, ChecksumErrors;       // SET and SEARCH_STEP: the caller's counts since the trial began
    ULONG Loaded;                           // SET and SEARCH_STEP: 1 while the caller loads the processor
    ULONGLONG ExpectedGeneration;           // every operation but READ
} BC250_CPU_REQUEST; // 56 bytes on Windows

// What a caller asks the case fan control for (Bc250Fan). Size is sizeof(BC250_FAN_REQUEST), for the same reason as
// BC250_CPU_REQUEST. tools/win/amdgpu_wddm_control mirrors it in src/Native.cs and its unit tests check the offsets
// against this text.
typedef struct _BC250_FAN_REQUEST {
    ULONG Size;                             // sizeof(BC250_FAN_REQUEST), 104
    ULONG Op;                               // BC250_FAN_OP_*
    ULONG Profile;                          // CURVE: enum bc250_fan_profile (0 custom, 1 standard, 2 quiet, 3 performance)
    ULONG Points;                           // CURVE with the custom profile: 2..8
    ULONG CurveC[BC250_FAN_CURVE_SLOTS];    // degrees C, rising
    ULONG CurvePct[BC250_FAN_CURVE_SLOTS];  // duty percent, never falling, 20..100
    ULONG FixedPct;                         // FIXED: 20..100
    ULONG LeaseMs;                          // CURVE (0 durable), FIXED and RENEW: 5000..300000
    ULONG Store;                            // BOARD and a durable CURVE: 1 makes it the choice of every start
    ULONG Reserved;                         // zero
    ULONGLONG ExpectedGeneration;           // every operation but READ
} BC250_FAN_REQUEST; // 104 bytes on Windows

static NTSTATUS TelemetryEscape(void *data, unsigned size);
static NTSTATUS TelemetryEscapeFlags(void *data, unsigned size, int hardware);
static NTSTATUS TelemetryAdapter(const WCHAR *wantedId, LUID *luid, ULONGLONG *dedicated);
static NTSTATUS TelemetryStatistics(D3DKMT_QUERYSTATISTICS *query);

// A limit can be "none": the development PC's RTX 4090 reports an aperture CommitLimit of 2^64-1 (2026-09-30).
static ULONGLONG SaturatingAdd(ULONGLONG a, ULONGLONG b)
{
    return a + b < a ? ~0ull : a + b;
}

// READ only: the overlay has no business confirming a DPM start. Never idles the scheduler, never reads a BAR.
//
// The caller's buffer decides the ABI, and that is what keeps the deployed callers working. bc250mon's
// Driver.cs and the control DLL's Native.cs both pass 160 bytes, the ABI 1 layout, and they were built
// against a header in which that was the whole structure. RUN_DPM grew to 192 bytes in 0.7.207 (the idle
// state), so a check against sizeof(BC250_ESCAPE_DPM) alone would refuse every one of those calls and a
// compile-time assertion on that size would stop the DLL being built at all. The driver takes either size
// with its own AbiVersion (driver/kmd/display.c), so this function asks with the ABI of the size it was
// given and never writes past it. A rebuilt caller that passes 192 gets the idle fields as well, and one that
// passes BC250_DPM_ABI3_SIZE (a BC250_ESCAPE_DPM_EX, 0.7.215) also gets the SMU metrics tail with the power reading.
// A driver before 0.7.215 refuses 248 bytes with STATUS_INVALID_PARAMETER; the caller then asks again with 160.
BC250_CONTROL_API LONG WINAPI Bc250Dpm(BC250_ESCAPE_DPM *data, ULONG bytes)
{
    NTSTATUS status;
    unsigned long abi;
    typedef char DpmAbiSizeCheck[(BC250_DPM_ABI1_SIZE == 160 && sizeof(BC250_ESCAPE_DPM) >= 160 &&
                                  sizeof(BC250_ESCAPE_DPM_EX) == BC250_DPM_ABI3_SIZE) ? 1 : -1];
    (void)sizeof(DpmAbiSizeCheck);
    if (!data || (bytes != BC250_DPM_ABI1_SIZE && bytes != sizeof(*data) && bytes != BC250_DPM_ABI3_SIZE))
        return (LONG)0xC000000D;
    abi = bytes == BC250_DPM_ABI1_SIZE ? BC250_DPM_ABI_1 : bytes == BC250_DPM_ABI3_SIZE ? BC250_DPM_ABI_3 : BC250_DPM_ABI;
    memset(data, 0, bytes);
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_DPM;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = abi;
    data->Op = BC250_DPM_OP_READ;
    status = TelemetryEscape(data, bytes);
    if (!NT_SUCCESS(status)) return status;         // a KMD before 0.7.175 refuses the command: DEVICE_NOT_READY
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_DPM ||
        data->AbiVersion != abi || data->Op != BC250_DPM_OP_READ)
        return (LONG)0xC000000D;
    return 0;
}

// The board's hardware monitor (BC250_ESCAPE_RUN_HWMON, KMD 0.7.213.1 and later): the fan speed, the duty
// read-back, the fan mode mask and the chip's own temperature channels, as the governor thread published them a
// second ago at most. Adapter-owned software snapshot, so NoAdapterSynchronization alone, like the DPM read: no
// port access on this path, no BAR access, no scheduler idle, so a sampler may call it while a game runs.
//
// No ABI fallback loop and no size negotiation: the escape was born at ABI 1 with one size. A driver older than
// 0.7.213.1 does not know command 27 and answers BC250_ESCAPE_STATUS_UNKNOWN_COMMAND, which the check below maps
// to 0xC00000BB (STATUS_NOT_SUPPORTED), the same answer the other snapshots give for a driver that is too old.
// The deployed 0.7.208.1 is such a driver: it stops at command 26, so 0xC00000BB from here is its correct
// answer and not a defect of the fan path. The reader was written as revision 208 on fan/read-nct6686 and
// reached a release in 0.7.213.1, which is why no 0.7.208.1 knows it.
BC250_CONTROL_API LONG WINAPI Bc250Hwmon(BC250_ESCAPE_HWMON *data, ULONG bytes)
{
    NTSTATUS status;
    typedef char HwmonAbiSizeCheck[(sizeof(BC250_ESCAPE_HWMON) == 216) ? 1 : -1];
    (void)sizeof(HwmonAbiSizeCheck);
    if (!data || bytes != sizeof(*data)) return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_HWMON;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = BC250_HWMON_ABI;
    data->Op = BC250_HWMON_OP_READ;
    status = TelemetryEscape(data, sizeof(*data));
    if (!NT_SUCCESS(status)) return status;
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_HWMON ||
        data->AbiVersion != BC250_HWMON_ABI || data->Op != BC250_HWMON_OP_READ)
        return (LONG)0xC000000D;
    return 0;
}

// The case fan control (BC250_ESCAPE_RUN_FAN, docs/design/fan.md Part B): READ for anybody, and BOARD, CURVE, FIXED
// and RENEW for an administrator with the Generation of a READ of the same start. Every operation is adapter-owned
// software state answered with NoAdapterSynchronization alone: a write leaves a request that the governor thread
// applies at its next step, so no escape of this surface touches a port. A driver that does not know command 30
// answers UNKNOWN_COMMAND, mapped to 0xC00000BB as for the other snapshots. A refused request returns its NtStatus,
// and the reply's Error names the rule (enum bc250_fan_error) even then.
BC250_CONTROL_API LONG WINAPI Bc250Fan(const BC250_FAN_REQUEST *request, BC250_ESCAPE_FAN *data, ULONG bytes)
{
    NTSTATUS status;
    ULONG op, i;
    typedef char FanAbiSizeCheck[(sizeof(BC250_ESCAPE_FAN) == 272 && sizeof(BC250_FAN_REQUEST) == 104) ? 1 : -1];
    (void)sizeof(FanAbiSizeCheck);
    if (!request || !data || bytes != sizeof(*data) || request->Size != sizeof(*request)) return (LONG)0xC000000D;
    op = request->Op;
    if (op > BC250_FAN_OP_RENEW || request->Points > BC250_FAN_CURVE_SLOTS || request->Reserved)
        return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_FAN;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = BC250_FAN_ABI;
    data->Op = op;
    if (op != BC250_FAN_OP_READ) {
        data->ExpectedGeneration = request->ExpectedGeneration;
        data->Profile = request->Profile;
        data->Points = request->Points;
        for (i = 0; i < request->Points; i++) {
            data->CurveC[i] = request->CurveC[i];
            data->CurvePct[i] = request->CurvePct[i];
        }
        data->FixedPct = request->FixedPct;
        data->LeaseMs = request->LeaseMs;
        data->Store = request->Store;
    }
    status = TelemetryEscape(data, sizeof(*data));
    if (!NT_SUCCESS(status)) return status;
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_FAN ||
        data->AbiVersion != BC250_FAN_ABI || data->Op != op)
        return (LONG)0xC000000D;
    return 0;
}

// The CU mode snapshot (READ, any caller) and the boot-guard confirmation of a pending 40 CU start (CONFIRM, an
// administrator with the Generation of a READ of this start). Both are adapter-owned software state answered with
// NoAdapterSynchronization alone (bc250kmd_escape.h), like the DPM read: no BAR access, no scheduler idle.
BC250_CONTROL_API LONG WINAPI Bc250CuMode(ULONG op, ULONGLONG expectedGeneration, BC250_ESCAPE_CU_MODE *data, ULONG bytes)
{
    NTSTATUS status;
    typedef char CuModeAbiSizeCheck[(sizeof(BC250_ESCAPE_CU_MODE) == 184) ? 1 : -1];
    (void)sizeof(CuModeAbiSizeCheck);
    if (!data || bytes != sizeof(*data) || (op != BC250_CU_MODE_OP_READ && op != BC250_CU_MODE_OP_CONFIRM))
        return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_CU_MODE;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = BC250_CU_MODE_ABI;
    data->Op = op;
    data->ExpectedGeneration = op == BC250_CU_MODE_OP_CONFIRM ? expectedGeneration : 0;
    status = TelemetryEscape(data, sizeof(*data));
    if (!NT_SUCCESS(status)) return status;
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_CU_MODE ||
        data->AbiVersion != BC250_CU_MODE_ABI || data->Op != op)
        return (LONG)0xC000000D;
    return 0;
}

// The operator's V/F curve (0.7.213): READ for the window's chart, and the trial operations behind one elevated
// action. Every operation is adapter-owned software state answered with NoAdapterSynchronization alone, like the
// DPM read above: the governor thread applies the curve at its next tick, so no escape of this surface touches a
// mailbox. A write needs an administrator (the KMD checks the caller's token itself) and the Generation of a READ
// of the same start, which is how a stale window cannot change a curve it has not seen.
//
// mv/points carry the candidate of a SET, so a caller never has to know where CandidateMv lies in the structure;
// every other operation ignores them. windowMs is the trial window of a SET (0: the driver's own 25 s).
BC250_CONTROL_API LONG WINAPI Bc250DpmCurve(ULONG op, ULONGLONG expectedGeneration, const ULONG *mv, ULONG points,
                                            ULONG windowMs, BC250_ESCAPE_DPM_CURVE *data, ULONG bytes)
{
    NTSTATUS status;
    ULONG i;
    typedef char CurveAbiSizeCheck[(sizeof(BC250_ESCAPE_DPM_CURVE) == 360) ? 1 : -1];
    (void)sizeof(CurveAbiSizeCheck);
    if (!data || bytes != sizeof(*data) || op > BC250_DPM_CURVE_OP_RESET) return (LONG)0xC000000D;
    if (op == BC250_DPM_CURVE_OP_SET && (!mv || points != BC250_DPM_CURVE_POINTS)) return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_DPM_CURVE;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = BC250_DPM_CURVE_ABI;
    data->Op = op;
    data->ExpectedGeneration = op == BC250_DPM_CURVE_OP_READ ? 0 : expectedGeneration;
    if (op == BC250_DPM_CURVE_OP_SET) {
        for (i = 0; i < BC250_DPM_CURVE_POINTS; i++) data->CandidateMv[i] = mv[i];
        data->TrialMs = windowMs;
    }
    status = TelemetryEscape(data, sizeof(*data));
    if (!NT_SUCCESS(status)) return status;          // a KMD before 0.7.213 refuses the command: DEVICE_NOT_READY
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_DPM_CURVE ||
        data->AbiVersion != BC250_DPM_CURVE_ABI || data->Op != op)
        return (LONG)0xC000000D;
    return 0;
}

// The CPU surface (0.7.213). One request structure instead of seven arguments, so that a new field is a version of
// this DLL and not a new export. READ is software state, and so is KEEP from 0.7.213 (it writes the registry and
// ends the trial, and sends nothing); every other operation sends mailbox messages on the firmware's queue 3 and
// therefore goes with HardwareAccess, an administrator and the Generation of a READ of the
// same start. READBACK asks for the administrator as well since 0.7.211: it holds the surface for some twenty
// messages on the shared mailbox, which an unprivileged loop must not be able to do. The driver owns the trial
// deadline and the revert, so a window that stops calling loses the trial and nothing else.
BC250_CONTROL_API LONG WINAPI Bc250Cpu(const BC250_CPU_REQUEST *request, BC250_ESCAPE_CPU *data, ULONG bytes)
{
    static int keepLegacy;          // a KEEP was refused without HardwareAccess: this driver is 0.7.212 or older
    BC250_ESCAPE_CPU sent;
    NTSTATUS status;
    ULONG op;
    typedef char CpuAbiSizeCheck[(sizeof(BC250_ESCAPE_CPU) == 296 && sizeof(BC250_CPU_REQUEST) == 56) ? 1 : -1];
    (void)sizeof(CpuAbiSizeCheck);
    if (!request || !data || bytes != sizeof(*data) || request->Size != sizeof(*request)) return (LONG)0xC000000D;
    op = request->Op;
    if (op > BC250_CPU_OP_SEARCH_STEP) return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_CPU;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = BC250_CPU_ABI;
    data->Op = op;
    data->ExpectedGeneration = op == BC250_CPU_OP_READ ? 0 : request->ExpectedGeneration;
    if (op == BC250_CPU_OP_SET) {
        data->Given = request->Given;
        data->MaxMHz = request->MaxMHz;
        data->UvSteps = request->UvSteps;
        data->TempC = request->TempC;
    }
    if (op == BC250_CPU_OP_SEARCH_BEGIN) data->UvSteps = request->UvSteps;
    if (op == BC250_CPU_OP_SET || op == BC250_CPU_OP_SEARCH_STEP) {
        data->WheaEvents = request->WheaEvents;
        data->ChecksumErrors = request->ChecksumErrors;
        data->Loaded = request->Loaded;
    }
    if (op == BC250_CPU_OP_CORES) data->CoreMask = request->CoreMask;
    if (op == BC250_CPU_OP_SET || op == BC250_CPU_OP_SEARCH_BEGIN) data->TrialMs = request->TrialMs;
    sent = *data;                                   // the request as built, for the retry below
    // KEEP sends no mailbox message, so it goes with the software flag word. A driver up to 0.7.212 refuses that
    // word at its gate (cpu.c: Status REFUSED, NtStatus STATUS_INVALID_PARAMETER, Version written first), so the
    // same request goes once more with the old word and this process keeps sending it that way, as
    // Bc250StartHealth does for CONFIRM. Without it a tester who installs this release and defers the device
    // restart loses the trial: the new DLL next to the still-loaded old driver would get nothing but refusals.
    if (op == BC250_CPU_OP_KEEP && keepLegacy) {
        status = TelemetryEscapeFlags(data, sizeof(*data), 1);
    } else {
        status = TelemetryEscapeFlags(data, sizeof(*data), op != BC250_CPU_OP_READ && op != BC250_CPU_OP_KEEP);
        if (op == BC250_CPU_OP_KEEP && NT_SUCCESS(status) &&
            data->Status == BC250_ESCAPE_STATUS_REFUSED && data->NtStatus == 0xC000000Dul) {
            keepLegacy = 1;
            *data = sent;
            status = TelemetryEscapeFlags(data, sizeof(*data), 1);
        }
    }
    if (!NT_SUCCESS(status)) return status;          // a KMD before 0.7.213 refuses the command: DEVICE_NOT_READY
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_CPU ||
        data->AbiVersion != BC250_CPU_ABI || data->Op != op)
        return (LONG)0xC000000D;
    return 0;
}

// Any adapter by hardware id prefix (NULL or empty: the BC-250), so the same code has a positive control on a
// GPU whose numbers Task Manager also shows.
BC250_CONTROL_API LONG WINAPI Bc250VideoMemory(const WCHAR *hardwareId, BC250_VIDEO_MEMORY *data, ULONG bytes)
{
    D3DKMT_QUERYSTATISTICS query;
    const D3DKMT_QUERYSTATISTICS_SEGMENT_INFORMATION *segment = &query.QueryResult.SegmentInformation;
    LUID luid;
    NTSTATUS status;
    typedef char VideoMemorySizeCheck[(sizeof(BC250_VIDEO_MEMORY) == 264) ? 1 : -1];
    (void)sizeof(VideoMemorySizeCheck);
    if (!data || bytes != sizeof(*data)) return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Size = sizeof(*data);
    status = TelemetryAdapter(hardwareId && hardwareId[0] ? hardwareId : BC250_DEFAULT_HWID, &luid,
                              &data->DedicatedVideoMemory);
    if (!NT_SUCCESS(status)) return status;
    data->LuidLow = luid.LowPart;
    data->LuidHigh = luid.HighPart;
    memset(&query, 0, sizeof(query));
    query.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    query.AdapterLuid = luid;
    status = TelemetryStatistics(&query);
    if (!NT_SUCCESS(status)) return status;
    data->Segments = query.QueryResult.AdapterInformation.NbSegments;
    if (data->Segments > BC250_VIDEO_MEMORY_MAX_SEGMENTS) return (LONG)0xC000000D;     // not a count to loop over
    for (ULONG i = 0; i < data->Segments; i++) {
        memset(&query, 0, sizeof(query));
        query.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
        query.AdapterLuid = luid;
        query.QuerySegment.SegmentId = i;
        status = TelemetryStatistics(&query);
        if (!NT_SUCCESS(status)) return status;
        if (segment->Aperture) {
            data->ApertureResident += segment->BytesResident;
            data->ApertureLimit = SaturatingAdd(data->ApertureLimit, segment->CommitLimit);
        } else {
            data->LocalResident += segment->BytesResident;
            data->LocalCommitted += segment->BytesCommitted;
            data->LocalLimit = SaturatingAdd(data->LocalLimit, segment->CommitLimit);
        }
        if (i < BC250_VIDEO_MEMORY_SEGMENTS) {
            if (segment->Aperture) data->ApertureMask |= 1u << i;
            data->Resident[i] = segment->BytesResident;
            data->Committed[i] = segment->BytesCommitted;
            data->Limit[i] = segment->CommitLimit;
        }
    }
    return 0;
}

// ---- telemetry: adapter access --------------------------------------------------------------------------------

static SRWLOCK g_TelemetryLock = SRWLOCK_INIT;
static BC250_ADAPTER g_TelemetryAdapters[16];      // FindAdapters' list, 40 KB: not on a caller's stack
static WCHAR g_TelemetryId[128], g_TelemetryPath[512];
static ULONGLONG g_TelemetryMiss;                   // GetTickCount64 of the last lookup that found nothing to open

// With g_TelemetryLock held. The path is resolved once per hardware id; a failed open of a cached path resolves
// it again at once (the device was replaced). An adapter that is not there, or does not open (another PC, or
// ours between a PnP stop and start), is looked for again at most every two seconds: one SetupAPI walk costs
// ~0.7 ms of CPU, a cached open plus four statistics queries ~6 us (development PC, 2026-09-30).
static NTSTATUS TelemetryOpenLocked(const WCHAR *wantedId, D3DKMT_OPENADAPTERFROMDEVICENAME *open)
{
    NTSTATUS status = (NTSTATUS)0xC000000E;         // STATUS_NO_SUCH_DEVICE
    for (int attempt = 0; attempt < 2; attempt++) {
        ULONGLONG now = GetTickCount64();
        int fresh = 0;
        if (!g_TelemetryPath[0] || _wcsicmp(g_TelemetryId, wantedId)) {
            int count, chosen = -1;
            if (!_wcsicmp(g_TelemetryId, wantedId) && now - g_TelemetryMiss < 2000) return status;
            count = FindAdapters(g_TelemetryAdapters, 16);
            for (int i = 0; i < count && chosen < 0; i++)
                if (MatchesHardwareId(g_TelemetryAdapters[i].HardwareId, wantedId)) chosen = i;
            wcsncpy_s(g_TelemetryId, 128, wantedId, _TRUNCATE);
            g_TelemetryPath[0] = 0;
            if (chosen < 0) { g_TelemetryMiss = now; return status; }
            wcscpy_s(g_TelemetryPath, 512, g_TelemetryAdapters[chosen].InterfacePath);
            fresh = 1;
        }
        memset(open, 0, sizeof(*open));
        open->pDeviceName = g_TelemetryPath;
        status = D3DKMTOpenAdapterFromDeviceName(open);
        if (NT_SUCCESS(status)) return status;
        g_TelemetryPath[0] = 0;
        if (fresh) { g_TelemetryMiss = now; return status; }
        g_TelemetryMiss = 0;                        // a cached path went stale: look again now
    }
    return status;
}

// hardware = 0: NoAdapterSynchronization alone, a software snapshot, which is what every read of this DLL sends
// and what the KMD demands of them. hardware = 1: HardwareAccess alone (the Level Two exclusion), for the one
// surface whose writes reach a mailbox, the CPU surface of 0.7.213 - its KEEP excepted, which reaches no mailbox
// and goes with hardware = 0 from 0.7.213. The KMD refuses any other combination, per
// operation, so a mistake here is a refusal and never a half-privileged escape.
static NTSTATUS TelemetryEscapeFlags(void *data, unsigned size, int hardware)
{
    D3DKMT_OPENADAPTERFROMDEVICENAME open;
    D3DKMT_CLOSEADAPTER close = { 0 };
    D3DKMT_ESCAPE escape = { 0 };
    NTSTATUS status;

    AcquireSRWLockExclusive(&g_TelemetryLock);
    status = TelemetryOpenLocked(BC250_DEFAULT_HWID, &open);
    if (NT_SUCCESS(status)) {
        escape.hAdapter = open.hAdapter;
        escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
        if (hardware) escape.Flags.HardwareAccess = 1;
        else escape.Flags.NoAdapterSynchronization = 1;
        escape.pPrivateDriverData = data;
        escape.PrivateDriverDataSize = size;
        status = D3DKMTEscape(&escape);
        close.hAdapter = open.hAdapter;
        D3DKMTCloseAdapter(&close);
    }
    ReleaseSRWLockExclusive(&g_TelemetryLock);
    return status;
}

static NTSTATUS TelemetryEscape(void *data, unsigned size)
{
    return TelemetryEscapeFlags(data, size, 0);
}

static NTSTATUS TelemetryAdapter(const WCHAR *wantedId, LUID *luid, ULONGLONG *dedicated)
{
    D3DKMT_OPENADAPTERFROMDEVICENAME open;
    D3DKMT_CLOSEADAPTER close = { 0 };
    D3DKMT_QUERYADAPTERINFO info = { 0 };
    D3DKMT_SEGMENTSIZEINFO sizes = { 0 };
    NTSTATUS status;

    AcquireSRWLockExclusive(&g_TelemetryLock);
    status = TelemetryOpenLocked(wantedId, &open);
    if (NT_SUCCESS(status)) {
        *luid = open.AdapterLuid;
        info.hAdapter = open.hAdapter;
        info.Type = KMTQAITYPE_GETSEGMENTSIZE;      // answered by dxgkrnl from the segment list, not by the KMD
        info.pPrivateDriverData = &sizes;
        info.PrivateDriverDataSize = sizeof(sizes);
        if (NT_SUCCESS(D3DKMTQueryAdapterInfo(&info))) *dedicated = sizes.DedicatedVideoMemorySize;
        close.hAdapter = open.hAdapter;
        D3DKMTCloseAdapter(&close);
    }
    ReleaseSRWLockExclusive(&g_TelemetryLock);
    return status;
}

static NTSTATUS TelemetryStatistics(D3DKMT_QUERYSTATISTICS *query)
{
    return D3DKMTQueryStatistics(query);
}

// ---- the control application's reads (tools/win/amdgpu_wddm_control) ------------------------------------------

// The GPU DWM interop decision of this start (driver/kmd/interop.c): software state, open to every caller. The control
// application (tools/win/amdgpu_wddm_control) shows from it whether the desktop composes through the GPU path.
BC250_CONTROL_API LONG WINAPI Bc250Interop(BC250_ESCAPE_INTEROP *data, ULONG bytes)
{
    NTSTATUS status;
    typedef char InteropAbiSizeCheck[(sizeof(BC250_ESCAPE_INTEROP) == 104) ? 1 : -1];
    (void)sizeof(InteropAbiSizeCheck);
    if (!data || bytes != sizeof(*data)) return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_INTEROP;
    data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
    data->AbiVersion = BC250_INTEROP_ABI;
    data->Op = BC250_INTEROP_OP_READ;
    status = TelemetryEscape(data, sizeof(*data));
    if (!NT_SUCCESS(status)) return status;
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE || data->NtStatus != 0)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_RUN_INTEROP ||
        data->AbiVersion != BC250_INTEROP_ABI || data->Op != BC250_INTEROP_OP_READ)
        return (LONG)0xC000000D;
    return 0;
}

// One page of the driver log ring from sequence `from` on, GET_LOG with NoAdapterSynchronization alone: answered by
// 0.7.184.1 and later without the adapter lock, so a bug report taken while a game runs does not stall it (BD-054).
// No HardwareAccess fallback and no LOG_SUMMARY here: both take the adapter lock. An older driver refuses with
// STATUS_DEVICE_NOT_READY, and the caller says so.
BC250_CONTROL_API LONG WINAPI Bc250LogRead(ULONG from, BC250_ESCAPE_LOG *data, ULONG bytes)
{
    NTSTATUS status;
    typedef char LogAbiSizeCheck[(sizeof(BC250_ESCAPE_LOG) == 60 + 64 * 168) ? 1 : -1];
    (void)sizeof(LogAbiSizeCheck);
    if (!data || bytes != sizeof(*data) || from == BC250_LOG_FROM_SUMMARY) return (LONG)0xC000000D;
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_GET_LOG;
    data->From = from;
    status = TelemetryEscape(data, sizeof(*data));
    if (!NT_SUCCESS(status)) return status;
    if (data->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) return (LONG)0xC00000BB;
    if (data->Status != BC250_ESCAPE_STATUS_DONE)
        return data->NtStatus ? (LONG)data->NtStatus : (LONG)0xC00000A3;
    if (data->Magic != BC250_ESCAPE_MAGIC || data->Command != BC250_ESCAPE_GET_LOG ||
        data->Returned > BC250_LOG_MAX_LINES)
        return (LONG)0xC000000D;
    return 0;
}

// ---- fan: the board's hardware monitor (BC250_ESCAPE_RUN_HWMON, docs/design/fan.md) ----------------------------
//
// One line per sample, in the same key=value shape the lab samplers already parse. The fan speed is the fastest
// tachometer that turns, because the chip exposes five channels on this board and only one of them has a fan on
// it; fan= names which one, counted from 1 as the Linux labels count. duty_pct is the duty READ-BACK, and
// duty_proven=0 says that no lab trial has yet shown that duty follow the fan that turns. tsi_c is the chip's own
// reading of the APU over SB-TSI, an independent second measurement of the temperature the SMU reports as Tctl;
// board_c is the first board thermistor.
//
// turning= and answered= are not the same question. answered= counts the present tachometers whose value THIS
// sample accepted, turning= those that report a speed above zero. "answered 4/5" means one channel was refused,
// which is what a second reader on this unarbitrated window looks like; a stopped fan reads 0 and answers.
// mode= and engine= are the two UNPROVEN registers as the START read them, not per sample, so they do not move
// inside a run. refusals= counts every refused value since the start.
static const char *HwmonReasonName(unsigned long reason)
{
    static const char *const names[] = { "ok", "gated", "base", "identity", "monitoring", "customer",
                                         "no-thread", "port" };
    return reason < ARRAYSIZE(names) ? names[reason] : "?";
}

static void HwmonLine(const BC250_ESCAPE_HWMON *h)
{
    unsigned long i, fan = 0, rpm = 0, duty = 0, turning = 0, present = 0, answered = 0;
    long tsi = 0, board = 0;
    int haveTsi = 0, haveBoard = 0;

    for (i = 0; i < BC250_HWMON_FAN_SLOTS; i++) {
        if (h->FanPresentMask & (1u << i)) present++;
        if (h->RpmValidMask & (1u << i)) answered++;
        if (h->Rpm[i] > rpm) { rpm = h->Rpm[i]; fan = i; }
        if (h->Rpm[i] != 0) turning++;
        if (h->DutyPermille[i] > duty) duty = h->DutyPermille[i];
    }
    for (i = 0; i < BC250_HWMON_TEMP_SLOTS; i++) {
        if (h->TemperatureSource[i] == 0) continue;
        if (h->TemperatureSource[i] == BC250_HWMON_SOURCE_APU) { tsi = h->TemperatureMc[i]; haveTsi = 1; }
        else if (!haveBoard) { board = h->TemperatureMc[i]; haveBoard = 1; }
    }
    printf("fan fan=%lu rpm=%lu turning=%lu/%lu answered=%lu/%lu duty_pct=%lu duty_proven=%d mode=0x%02lX "
           "engine=0x%02lX ", fan + 1, rpm, turning, present, answered, present, (duty + 5) / 10,
           (h->Flags & BC250_HWMON_FLAG_DUTY_PROVEN) ? 1 : 0, h->ModeMask, h->Engine);
    if (haveTsi) printf("tsi_c=%.1f ", tsi / 1000.0); else printf("tsi_c=n/a ");
    if (haveBoard) printf("board_c=%.1f ", board / 1000.0); else printf("board_c=n/a ");
    printf("age_ms=%lu fresh=%d valid=%d stopped=%d reason=%s samples=%llu errors=%llu retries=%llu "
           "refusals=%llu\n",
           h->AgeMs, (h->Flags & BC250_HWMON_FLAG_FRESH) ? 1 : 0, (h->Flags & BC250_HWMON_FLAG_VALID) ? 1 : 0,
           (h->Flags & BC250_HWMON_FLAG_STOPPED) ? 1 : 0, HwmonReasonName(h->Reason),
           h->Samples, h->Errors, h->Retries, h->Refusals);
}

// The names of the fan control's enumerations (driver/shim/include/bc250_fan.h), for the "fanctl" line.
static const char *FanName(unsigned long value, const char *const *names, unsigned long count)
{
    return value < count ? names[value] : "?";
}

static const char *const g_fanStates[] = { "off", "board", "curve", "fixed", "emergency", "doubt", "fault" };
static const char *const g_fanModes[] = { "board", "curve", "fixed" };
static const char *const g_fanProfiles[] = { "custom", "standard", "quiet", "performance" };
static const char *const g_fanReasons[] = { "none", "user", "stop", "power", "unload", "watchdog", "lease",
                                            "temperature", "reader", "handshake", "readback", "mode", "stopped",
                                            "disabled", "bugcheck" };
static const char *const g_fanErrors[] = { "ok", "mode", "profile", "points", "temperature", "duty", "lease" };
static const char *const g_fanGates[] = { "ok", "setting", "reader", "chip" };

// One line of the fan control's state. The "fan " line above it stays as it was: the lab samplers read it.
static void FanCtlLine(const BC250_ESCAPE_FAN *f)
{
    unsigned long i;

    printf("fanctl state=%s mode=%s profile=%s target_pct=%lu applied_pct=%lu raw=%lu readback=%lu rpm=%lu "
           "guard_c=%.1f enabled=%d controlling=%d emergency=%d leased=%d lease_ms=%lu fault=%d paused=%d "
           "held_back=%d gate=%s reason=%s doubt=%s takeovers=%llu handbacks=%llu writes=%llu failures=%llu "
           "emergencies=%llu lease_expiries=%llu watchdog=%llu curve=",
           FanName(f->State, g_fanStates, ARRAYSIZE(g_fanStates)), FanName(f->Mode, g_fanModes, ARRAYSIZE(g_fanModes)),
           FanName(f->Profile, g_fanProfiles, ARRAYSIZE(g_fanProfiles)), f->TargetPct, f->AppliedPct, f->WrittenRaw,
           f->ReadbackRaw, f->Rpm, f->GuardMc / 1000.0, (f->Flags & BC250_FAN_FLAG_ENABLED) ? 1 : 0,
           (f->Flags & BC250_FAN_FLAG_CONTROLLING) ? 1 : 0, (f->Flags & BC250_FAN_FLAG_EMERGENCY) ? 1 : 0,
           (f->Flags & BC250_FAN_FLAG_LEASED) ? 1 : 0, f->LeaseMs, (f->Flags & BC250_FAN_FLAG_FAULT) ? 1 : 0,
           (f->Flags & BC250_FAN_FLAG_PAUSED) ? 1 : 0, (f->Flags & BC250_FAN_FLAG_HELD_BACK) ? 1 : 0,
           FanName(f->Gate, g_fanGates, ARRAYSIZE(g_fanGates)), FanName(f->Reason, g_fanReasons, ARRAYSIZE(g_fanReasons)),
           FanName(f->DoubtReason, g_fanReasons, ARRAYSIZE(g_fanReasons)), f->Takeovers, f->Handbacks, f->Writes,
           f->Failures, f->Emergencies, f->LeaseExpiries, f->WatchdogFires);
    for (i = 0; i < f->Points && i < BC250_FAN_CURVE_SLOTS; i++)
        printf("%s%lu:%lu", i ? "," : "", f->CurveC[i], f->CurvePct[i]);
    if (f->Points == 0) printf("none");
    printf(" saved_mode=0x%02lX saved_target=%lu\n", f->SavedMode, f->SavedTarget);
}

// "fan auto [store]", "fan curve [standard|quiet|performance] [store]", "fan set <pct> <seconds>",
// "fan renew <seconds>". Each one reads first for the Generation, sends the request and prints the state the driver
// answered with. The governor thread applies the request at its next step, so a second "fan" a second later shows it
// in the chip. "store" makes "auto" or a durable curve the choice of every start; a fixed duty is never stored.
static int FanControl(int argc, WCHAR **argv)
{
    BC250_FAN_REQUEST r;
    BC250_ESCAPE_FAN f;
    const WCHAR *verb = argv[2];
    LONG status;
    int next = 3;

    memset(&r, 0, sizeof(r));
    r.Size = sizeof(r);
    r.Op = BC250_FAN_OP_READ;
    status = Bc250Fan(&r, &f, sizeof(f));
    if (status < 0) { PrintStatus("fanctl", status); return 1; }
    r.ExpectedGeneration = f.Generation;
    if (!_wcsicmp(verb, L"auto")) {
        r.Op = BC250_FAN_OP_BOARD;
    } else if (!_wcsicmp(verb, L"curve")) {
        r.Op = BC250_FAN_OP_CURVE;
        r.Profile = 1;      // BC250_FAN_PROFILE_STANDARD
        if (argc > next && _wcsicmp(argv[next], L"store")) {
            if (!_wcsicmp(argv[next], L"standard")) r.Profile = 1;
            else if (!_wcsicmp(argv[next], L"quiet")) r.Profile = 2;
            else if (!_wcsicmp(argv[next], L"performance")) r.Profile = 3;
            else { fprintf(stderr, "fan curve: standard, quiet or performance\n"); return 2; }
            next++;
        }
    } else if (!_wcsicmp(verb, L"set")) {
        if (argc != 5) { fprintf(stderr, "usage: fan set <percent 20..100> <seconds 5..300>\n"); return 2; }
        r.Op = BC250_FAN_OP_FIXED;
        r.FixedPct = wcstoul(argv[3], NULL, 10);
        r.LeaseMs = wcstoul(argv[4], NULL, 10) * 1000u;
        next = 5;
    } else if (!_wcsicmp(verb, L"renew")) {
        if (argc != 4) { fprintf(stderr, "usage: fan renew <seconds 5..300>\n"); return 2; }
        r.Op = BC250_FAN_OP_RENEW;
        r.LeaseMs = wcstoul(argv[3], NULL, 10) * 1000u;
        next = 4;
    } else {
        fprintf(stderr, "fan: auto [store] | curve [standard|quiet|performance] [store] | set <pct> <s> | renew <s>\n");
        return 2;
    }
    if (argc > next && !_wcsicmp(argv[next], L"store") && (r.Op == BC250_FAN_OP_BOARD || r.Op == BC250_FAN_OP_CURVE)) {
        r.Store = 1;
        next++;
    }
    if (argc > next) { fprintf(stderr, "fan %ls: unexpected argument %ls\n", verb, argv[next]); return 2; }
    status = Bc250Fan(&r, &f, sizeof(f));
    if (status < 0) {
        PrintStatus("fanctl", status);
        if (f.Error) fprintf(stderr, "fanctl refused: %s\n", FanName(f.Error, g_fanErrors, ARRAYSIZE(g_fanErrors)));
        if (f.Gate) fprintf(stderr, "fanctl gate: %s\n", FanName(f.Gate, g_fanGates, ARRAYSIZE(g_fanGates)));
        return 1;
    }
    FanCtlLine(&f);
    return 0;
}

// "fan [count [interval ms]]". The interval has a floor of one second, because the chip caches its registers for
// about that long and the driver samples at exactly that rate: a faster poll returns the same snapshot and only
// spends escapes. Each sample adds the fan control's state; a driver without it (0xC00000BB) is reported once.
static int Fan(int argc, WCHAR **argv)
{
    BC250_ESCAPE_HWMON h;
    BC250_FAN_REQUEST r;
    BC250_ESCAPE_FAN f;
    unsigned long count = 1, interval = 1000, i;
    LONG status;

    if (argc >= 3 && !iswdigit(argv[2][0])) return FanControl(argc, argv);
    if (argc >= 3) count = wcstoul(argv[2], NULL, 0);
    if (argc >= 4) interval = wcstoul(argv[3], NULL, 0);
    if (count == 0) count = 1;
    if (interval < 1000) interval = 1000;
    memset(&r, 0, sizeof(r));
    r.Size = sizeof(r);
    r.Op = BC250_FAN_OP_READ;
    for (i = 0; i < count; i++) {
        if (i) Sleep(interval);
        status = Bc250Hwmon(&h, sizeof(h));
        if (status < 0) { PrintStatus("fan", status); return 1; }
        if (i == 0)
            printf("driver 0x%08lX, base 0x%04lX, ec %lu.%lu build %02lu/%02lu/%02lu, customer 0x%04lX, "
                   "fans 0x%02lX duties 0x%02lX, generation %llu\n", h.Version, h.BasePort, h.EcVersion >> 8,
                   h.EcVersion & 0xFF, (h.EcBuild >> 8) & 0xFF, h.EcBuild & 0xFF, (h.EcBuild >> 16) & 0xFF,
                   h.CustomerId, h.FanPresentMask, h.DutyPresentMask, h.Generation);
        HwmonLine(&h);
        status = Bc250Fan(&r, &f, sizeof(f));
        if (status < 0) { if (i == 0) PrintStatus("fanctl", status); }
        else FanCtlLine(&f);
    }
    return 0;
}

// "telemetry [count [interval ms]]": what the monitor's GPU line is made of, one line per sample.
// "vram [hardware-id]": the segment statistics of any adapter, one line per segment.
static int Telemetry(int argc, wchar_t **argv)
{
    BC250_ESCAPE_DPM_EX x;
    BC250_VIDEO_MEMORY m;
    unsigned long count = argc >= 3 ? wcstoul(argv[2], NULL, 10) : 1, interval = argc >= 4 ? wcstoul(argv[3], NULL, 10) : 1000;
    ULONG dpmBytes = sizeof(x);     // ABI 3; the ABI 2 structure after a driver before 0.7.215 refused it
    LONG status;
    int failed = 0;
    if (count == 0) count = 1;
    for (unsigned long i = 0; i < count; i++) {
        const BC250_ESCAPE_DPM *d = &x.Dpm;
        if (i) Sleep(interval);
        status = Bc250Dpm(&x.Dpm, dpmBytes);
        if (status == (LONG)0xC000000D && dpmBytes == sizeof(x)) status = Bc250Dpm(&x.Dpm, dpmBytes = sizeof(x.Dpm));
        failed |= status < 0;
        if (status < 0) PrintStatus("dpm", status);
        else {
            printf("dpm version=0x%08lX flags=0x%lX temperature_c=%.1f%s gfx_mhz=%lu%s busy_pct=%.1f avg_pct=%.1f src=%s "
                   "submit_pct=%.1f", d->Version, d->Flags, d->TemperatureMc / 1000.0,
                   (d->Flags & BC250_DPM_FLAG_TEMPERATURE) ? "" : "(stale)", d->ObservedMHz,
                   (d->Flags & BC250_DPM_FLAG_CLOCK) ? "" : "(stale)", d->BusyPermille / 10.0, d->BusyAvgPermille / 10.0,
                   (d->Flags & BC250_DPM_FLAG_HW_BUSY) ? "grbm" : "submit", d->SubmitBusyPermille / 10.0);
            // The SMU's package power (0.7.215): only from a fresh table, so a sampler never logs an old value.
            if (dpmBytes == sizeof(x) && (d->Flags & BC250_DPM_FLAG_POWER))
                printf(" power_w=%.1f power_avg_w=%.1f", x.Metrics.SocketPowerMw / 1000.0, x.Metrics.SocketPowerAvgMw / 1000.0);
            printf("\n");
        }
        {
            // The board's hardware monitor, one line per sample, so the lab samplers pick the fan up with no new
            // process and no new session. A driver before 0.7.208.1 answers 0xC00000BB; that is reported once and
            // does not fail the sampler, because the fan reading is information and not the measurement this
            // command exists for.
            BC250_ESCAPE_HWMON h;
            LONG fanStatus = Bc250Hwmon(&h, sizeof(h));
            if (fanStatus < 0) { if (i == 0) PrintStatus("fan", fanStatus); }
            else HwmonLine(&h);
        }
        status = Bc250VideoMemory(NULL, &m, sizeof(m));
        failed |= status < 0;
        if (status < 0) PrintStatus("vram", status);
        else
            printf("vram segments=%lu local_resident_mb=%llu local_limit_mb=%llu dedicated_mb=%llu aperture_resident_mb=%llu "
                   "aperture_limit_mb=%llu\n", m.Segments, m.LocalResident >> 20, m.LocalLimit >> 20,
                   m.DedicatedVideoMemory >> 20, m.ApertureResident >> 20, m.ApertureLimit >> 20);
    }
    return failed ? 1 : 0;
}

static int SegmentSizes(const WCHAR *wantedId, D3DKMT_SEGMENTSIZEINFO *sizes);

static int VideoMemory(const WCHAR *wantedId)
{
    BC250_VIDEO_MEMORY m;
    LONG status = Bc250VideoMemory(wantedId, &m, sizeof(m));
    if (status < 0) { PrintStatus("vram", status); return 1; }
    printf("# adapter luid %08lX-%08lX, %lu segment(s), dedicated %llu bytes (KMTQAITYPE_GETSEGMENTSIZE)\n",
           (unsigned long)m.LuidHigh, m.LuidLow, m.Segments, m.DedicatedVideoMemory);
    for (ULONG i = 0; i < m.Segments && i < BC250_VIDEO_MEMORY_SEGMENTS; i++)
        printf("segment %lu %-8s resident %llu committed %llu limit %llu\n", i,
               (m.ApertureMask & (1u << i)) ? "aperture" : "memory", m.Resident[i], m.Committed[i], m.Limit[i]);
    printf("memory   resident %llu committed %llu limit %llu\naperture resident %llu limit %llu\n",
           m.LocalResident, m.LocalCommitted, m.LocalLimit, m.ApertureResident, m.ApertureLimit);
    // Memory manager stage 1d (0.7.216.8): the two sizes the line above has no room for, and the reason a
    // summed aperture limit can read 2^64-1 (dxgkrnl's implicit system-memory segment has no limit).
    // SharedSystemMemory is the pool the NON_LOCAL budget is taken from. A separate query, so the
    // BC250_VIDEO_MEMORY layout the monitor and the control application share stays as it is.
    {
        D3DKMT_SEGMENTSIZEINFO sizes;
        if (SegmentSizes(wantedId && wantedId[0] ? wantedId : BC250_DEFAULT_HWID, &sizes))
            printf("sizes dedicated video %llu dedicated system %llu shared system %llu (KMTQAITYPE_GETSEGMENTSIZE)\n",
                   sizes.DedicatedVideoMemorySize, sizes.DedicatedSystemMemorySize, sizes.SharedSystemMemorySize);
        else printf("sizes not answered (KMTQAITYPE_GETSEGMENTSIZE)\n");
        for (ULONG i = 0; i < m.Segments && i < BC250_VIDEO_MEMORY_SEGMENTS; i++)
            if ((m.ApertureMask & (1u << i)) && m.Limit[i] == ~0ull)
                printf("# segment %lu has no commit limit, so the summed aperture limit saturates\n", i);
    }
    return 0;
}

// ---- budget: one process's memory budget and residency (memory manager stage 1d, 0.7.216.8) ----------------------
//
// dxgkrnl's own view of one process on one adapter: per segment group (LOCAL, NON_LOCAL) the Budget, the bytes the
// process Requested and the Usage VidMm grants it, and Demoted[] - bytes VidMm placed in a lower-preference segment
// than the allocation asked for, by priority class (D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP, WDDM 2.1,
// d3dkmthk.h WDK 26100 line 3964). Then per segment the bytes the process has committed there
// (D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT). The WDK marks the group structure "reserved for system use"; this reads
// it as Task Manager and Process Explorer do, for measurement only. Nothing here reaches the KMD.

static int SegmentSizes(const WCHAR *wantedId, D3DKMT_SEGMENTSIZEINFO *sizes)
{
    D3DKMT_OPENADAPTERFROMDEVICENAME open;
    D3DKMT_CLOSEADAPTER close = { 0 };
    D3DKMT_QUERYADAPTERINFO info = { 0 };
    int ok = 0;
    memset(sizes, 0, sizeof(*sizes));
    AcquireSRWLockExclusive(&g_TelemetryLock);
    if (NT_SUCCESS(TelemetryOpenLocked(wantedId, &open))) {
        info.hAdapter = open.hAdapter;
        info.Type = KMTQAITYPE_GETSEGMENTSIZE;
        info.pPrivateDriverData = sizes;
        info.PrivateDriverDataSize = sizeof(*sizes);
        ok = NT_SUCCESS(D3DKMTQueryAdapterInfo(&info));
        close.hAdapter = open.hAdapter;
        D3DKMTCloseAdapter(&close);
    }
    ReleaseSRWLockExclusive(&g_TelemetryLock);
    return ok;
}

// A decimal pid, or the first process whose image name matches (case-insensitive). 0: none.
static DWORD BudgetProcessId(const WCHAR *text)
{
    PROCESSENTRY32W entry;
    HANDLE snapshot;
    WCHAR *end = NULL;
    unsigned long pid = wcstoul(text, &end, 10);
    DWORD found = 0;
    if (end && end != text && *end == 0) return (DWORD)pid;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        if (!_wcsicmp(entry.szExeFile, text)) found = entry.th32ProcessID;
    CloseHandle(snapshot);
    return found;
}

static int Budget(const WCHAR *process, const WCHAR *wantedId)
{
    static const char *const groups[2] = { "local", "non-local" };
    BC250_VIDEO_MEMORY m;
    D3DKMT_QUERYSTATISTICS query;
    LUID luid;
    HANDLE handle;
    DWORD pid = BudgetProcessId(process);
    LONG status;
    int failed = 0;

    if (!pid) { fprintf(stderr, "budget: no process %ls\n", process); return 1; }
    status = Bc250VideoMemory(wantedId, &m, sizeof(m));
    if (status < 0) { PrintStatus("budget", status); return 1; }
    luid.LowPart = m.LuidLow; luid.HighPart = m.LuidHigh;
    // dxgkrnl wants PROCESS_QUERY_INFORMATION: with a PROCESS_QUERY_LIMITED_INFORMATION handle every process query
    // answered 0xC000000D (development PC, explorer.exe on the RTX 4090, 2026-10-07). The limited right stays as
    // the fallback so that a refusal is a printed status, not a silent nothing.
    handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!handle) handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) { fprintf(stderr, "budget: OpenProcess(%lu) failed, error %lu\n", pid, GetLastError()); return 1; }
    printf("# process %lu (%ls) on adapter luid %08lX-%08lX\n", pid, process, (unsigned long)m.LuidHigh, m.LuidLow);
    for (int g = 0; g < 2; g++) {
        const D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP_INFORMATION *r =
            &query.QueryResult.ProcessSegmentGroupInformation;
        memset(&query, 0, sizeof(query));
        query.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
        query.AdapterLuid = luid;
        query.hProcess = handle;
        query.QueryProcessSegmentGroup = g ? D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL : D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        status = D3DKMTQueryStatistics(&query);
        if (!NT_SUCCESS(status)) { printf("group %s status 0x%08lX\n", groups[g], (unsigned long)status); failed = 1; continue; }
        printf("group %-9s budget %llu requested %llu usage %llu demoted", groups[g],
               r->Budget, r->Requested, r->Usage);
        for (int c = 0; c < (int)(sizeof(r->Demoted) / sizeof(r->Demoted[0])); c++) printf(" %llu", r->Demoted[c]);
        printf("\n");
    }
    for (ULONG i = 0; i < m.Segments && i < BC250_VIDEO_MEMORY_SEGMENTS; i++) {
        const D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_INFORMATION *s = &query.QueryResult.ProcessSegmentInformation;
        memset(&query, 0, sizeof(query));
        query.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT;
        query.AdapterLuid = luid;
        query.hProcess = handle;
        query.QueryProcessSegment.SegmentId = i;
        status = D3DKMTQueryStatistics(&query);
        if (!NT_SUCCESS(status)) { printf("segment %lu status 0x%08lX\n", i, (unsigned long)status); failed = 1; continue; }
        printf("segment %lu %-8s committed %llu evicted-in-period %lu\n", i,
               (m.ApertureMask & (1u << i)) ? "aperture" : "memory", s->BytesCommitted,
               s->NbReferencedAllocationEvictedInPeriod);
    }
    CloseHandle(handle);
    return failed;
}

// `read <name>`: a register of the driver's named READ_REG list (gen_regs.py EXTRA_READS, BC250_REG_READ_NAMES), so
// that an operator never types an offset. Accepted forms: NAME, mmNAME, IP.NAME and IP:NAME, any case. A text that
// is a hex number (with or without 0x) is an offset, as before.
static const struct { const char *Ip, *Name; unsigned long Offset; } g_RegNames[] = {
#define BC250_REG_NAME_ROW(ip, name, offset) { ip, name, offset },
    BC250_REG_READ_NAMES(BC250_REG_NAME_ROW)
#undef BC250_REG_NAME_ROW
};

static int IsHexText(const WCHAR *text)
{
    if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) text += 2;
    if (!*text) return 0;
    for (; *text; text++) if (!iswxdigit(*text)) return 0;
    return 1;
}

static int RegisterByName(const WCHAR *text, unsigned long *offset)
{
    char name[128], ip[16] = "";
    const char *bare;
    size_t i, n = wcslen(text);
    char *dot;

    if (n == 0 || n >= sizeof(name)) return 0;
    for (i = 0; i <= n; i++) {
        if (text[i] > 0x7E) return 0;
        name[i] = (char)text[i];
    }
    bare = name;
    dot = strpbrk(name, ".:");
    if (dot) {
        if ((size_t)(dot - name) >= sizeof(ip)) return 0;
        memcpy(ip, name, (size_t)(dot - name));
        ip[dot - name] = 0;
        bare = dot + 1;
    }
    if ((bare[0] == 'm' || bare[0] == 'M') && (bare[1] == 'm' || bare[1] == 'M')) {
        // mmNAME is regcalc's spelling; a register whose own name starts with MM keeps its prefix (the second try).
        for (i = 0; i < sizeof(g_RegNames) / sizeof(g_RegNames[0]); i++)
            if (!_stricmp(g_RegNames[i].Name, bare + 2) && (!ip[0] || !_stricmp(g_RegNames[i].Ip, ip))) {
                *offset = g_RegNames[i].Offset;
                return 1;
            }
    }
    for (i = 0; i < sizeof(g_RegNames) / sizeof(g_RegNames[0]); i++)
        if (!_stricmp(g_RegNames[i].Name, bare) && (!ip[0] || !_stricmp(g_RegNames[i].Ip, ip))) {
            *offset = g_RegNames[i].Offset;
            return 1;
        }
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
    if (!write && !IsHexText(offsetText)) {
        if (!RegisterByName(offsetText, &data.RegOffset)) {
            fprintf(stderr, "%ls is neither a hex offset nor a name on the driver's named read list"
                            " (gen_regs.py EXTRA_READS)\n", offsetText);
            return 2;
        }
        printf("%ls = 0x%05lX\n", offsetText, data.RegOffset);
    } else {
        data.RegOffset = wcstoul(offsetText, &end, 16);
        if (*end) { fprintf(stderr, "offset %ls is not a hex number\n", offsetText); return 2; }
    }
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
    // Both registers hold the total minus one, as display_timing.h reads them: print the field with the
    // Linux reference next to it, then the mode it means. The bare name "h_total" on the field value put a
    // 2079 by 1234 mode into a write-up once (E54).
    printf("             OTG_H_TOTAL field %lu / 2079, OTG_V_TOTAL field %lu / 1234 (h_total %lu, v_total %lu)\n",
           d.Otg0HTotal, d.Otg0VTotal, d.Otg0HTotal + 1, d.Otg0VTotal + 1);
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
//
// "log summary only" starts at the first line the summary itself wrote (BC250_LOG_FROM_SUMMARY): the summary and
// whatever the driver logged after it, two or three pages instead of the whole ring. It is the form for a caller
// that polls, such as the overlay. The last line counts the escapes of each kind, so that a poller can see that
// only the summary took the adapter lock (BD-054: a CLI from before 0.7.184.1 sent every page with HardwareAccess,
// and the overlay's poll of it stalled a running game for 280-420 ms every 5 s).

static int Log(const WCHAR *fromText, int summary)
{
    static BC250_ESCAPE_LOG log;        // 10 KB: a static, not a frame this tool has no reason to grow
    unsigned long from = 0, printed = 0, block = 0;     // block: the summary block being paged, 0 = the ring
    int first = 1;
    NTSTATUS status;
    WCHAR *end;

    if (fromText != NULL && summary && !_wcsicmp(fromText, L"only")) {
        from = BC250_LOG_FROM_SUMMARY;
    } else if (fromText != NULL) {
        from = wcstoul(fromText, &end, 10);
        // wcstoul takes "-1" and returns 0xFFFFFFFF, which is BC250_LOG_FROM_SUMMARY: a sentinel is not a number to type.
        if (*end || fromText[0] == L'-' || from == BC250_LOG_FROM_SUMMARY) {
            fprintf(stderr, "log [from] | log summary [from | only], where from is a decimal sequence number, not %ls\n",
                    fromText);
            return 2;
        }
    }
    for (;;) {
        memset(&log, 0, sizeof(log));
        log.Magic = BC250_ESCAPE_MAGIC;
        log.Command = (summary && first) ? BC250_ESCAPE_LOG_SUMMARY : BC250_ESCAPE_GET_LOG;
        log.From = from;
        // LOG_SUMMARY keeps HardwareAccess; GET_LOG pages go without adapter synchronization (ReadEscapeOn).
        if (SendReadEscape(BC250_DEFAULT_HWID, log.Command, &log, sizeof(log), &status)) return 1;
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
        // The sentinel is no position to compare with: the driver answers the sequence it actually read from.
        if (from == BC250_LOG_FROM_SUMMARY) from = log.From;
        // BD-097: a read inside the summary space pages one block, and the driver holds one block at a time. An
        // empty page whose SummaryFrom names another block is a block that a newer summary replaced, not the end
        // of this one, so a short block is never printed as if it were whole. A driver before 0.7.216.23 answers
        // SummaryFrom 0 for a page read and says nothing of the kind.
        if (from >= BC250_LOG_SUMMARY_SEQ)
            block = BC250_LOG_SUMMARY_SEQ +
                    (from - BC250_LOG_SUMMARY_SEQ) / BC250_LOG_SUMMARY_LINES * BC250_LOG_SUMMARY_LINES;
        if (log.Returned == 0 && block != 0 && log.SummaryFrom >= BC250_LOG_SUMMARY_SEQ && log.SummaryFrom != block)
            printf("             the block beside the ring was replaced by a newer summary after %lu lines; the "
                   "one held now reads from %lu\n", printed, log.SummaryFrom);
        if (log.Returned == 0 || log.Next <= from) break;   // the end, or a driver that is not moving on
        from = log.Next;
        // A driver that keeps logging while we read would keep us here: the ring is 1024 lines, so anything past
        // a few times that is a live stream rather than a trail, and whoever wants more can ask again.
        if (printed > 4 * log.RingLines) { printf("             stopped at %lu lines; ask again from %lu\n", printed, from); break; }
    }
    printf("             %lu lines printed\n", printed);
    printf("             escapes: %lu without adapter synchronization, %lu with HardwareAccess\n",
           g_SoftReads, g_HardReads);
    return 0;
}

// ---- journal: the paging journal (BC250_ESCAPE_GET_PAGING_JOURNAL, docs/design/paging-journal.md) ---------------
//
// "journal [from]" prints the driver's paging journal from record index `from` (default: the oldest record still
// held), one line per record: index, driver time in seconds since boot (interrupt time), kind, page table
// level/index/count/valid entries, the GPU VA, the allocation handle, the offset or byte count, the position in
// the paging buffer, the OS fence and the SDMA sequence that carried it.
//
// From 0.7.193.1 two kinds reuse those four words for identity, and the tail of the line spells it out: a
// `destroy` names the process and thread that called it and the process that created the allocation, and the
// new `gfx-submit` kind names the context, its process and the IB1 and root of one GFX job (KMD193, written for
// the 0x116 of trial 245, where nothing in the journal could say whose job had faulted).

static const char *const g_JournalKind[] = { "?", "update-cpu", "update-gpu", "vfill", "vtransfer", "flush-tlb",
                                             "destroy", "transfer", "fill", "gfx-submit" };

static void PrintJournalRecord(const BC250_ESCAPE_PAGING_JOURNAL *journal, unsigned long i)
{
    const BC250_PAGING_JOURNAL_RECORD *r = &journal->Records[i];
    const char *kind = r->Kind < sizeof(g_JournalKind) / sizeof(g_JournalKind[0]) ? g_JournalKind[r->Kind] : "?";
    char segments[64] = "";
    char identity[160] = "";

    // KMD193 (0.7.193.1 and later) puts identity in the words each kind left unused, so the same L/i/n/v
    // columns mean something else for these two kinds. Print what they mean rather than four bare numbers.
    // An older driver leaves them zero, which prints as a destroy with no process and reads as "not recorded".
    if (r->Kind == BC250_PJ_DESTROY_ALLOCATION && (r->Level | r->Index | r->Count | r->Valid) != 0)
        snprintf(identity, sizeof(identity), " by pid %lu tid %lu, created by pid %lu, bc2a v%lu gem 0x%llX",
                 r->Level, r->Index, r->Count, r->Valid, r->Dma);
    else if (r->Kind == BC250_PJ_GFX_SUBMIT)
        // Valid: the VMID, from 0.7.214.1. 0 = an earlier driver, whose WDDM jobs all ran at VMID 1.
        snprintf(identity, sizeof(identity), " node %lu ctx 0x%llX pid %lu%s%s ib 0x%llX root 0x%llX vmid %lu%s",
                 r->Level, r->Allocation, r->Index, (r->Count & BC250_PJ_CTX_UMD) ? " umd" : "",
                 (r->Count & BC250_PJ_CTX_SYSTEM) ? " system" : "", r->Va, r->Offset,
                 r->Valid != 0 ? r->Valid : 1ul, r->Valid != 0 ? "" : " (not recorded)");
    else if (r->Flags & BC250_PJ_FLAG_PROCESS)
        snprintf(identity, sizeof(identity), " hprocess 0x%llX (no allocation)", r->Allocation);

    // UPDATE records of 0.7.183.1 and later carry the PTE segments of their valid entries in Flags bits 16-31
    // (BC250_PJ_FLAG_SEGMENT): " seg 0,1" = system memory and segment 1. Older drivers leave the bits zero.
    if ((r->Kind == BC250_PJ_UPDATE_CPU || r->Kind == BC250_PJ_UPDATE_GPU) && (r->Flags & BC250_PJ_FLAG_SEGMENT_MASK)) {
        const char *separator = " seg ";
        size_t used = 0;
        for (unsigned s = 0; s < 16 && used < sizeof(segments); s++) {
            if (!(r->Flags & (1u << (BC250_PJ_FLAG_SEGMENT_SHIFT + s)))) continue;
            if (s == 15) used += (size_t)snprintf(segments + used, sizeof(segments) - used, "%s15+", separator);
            else used += (size_t)snprintf(segments + used, sizeof(segments) - used, "%s%u", separator, s);
            separator = ",";
        }
    }
    printf("%8llu %14.6f %-10s L%lu i%-3lu n%-3lu v%-3lu va 0x%012llX alloc 0x%016llX off 0x%llX dma 0x%llX "
           "fence %lu seq %lu flags 0x%lX%s%s%s\n",
           journal->Next - journal->Returned + i, (double)r->Time / 1e7, kind, r->Level, r->Index, r->Count,
           r->Valid, r->Va, r->Allocation, r->Offset, r->Dma, r->Fence, r->Seq, r->Flags,
           (r->Flags & BC250_PJ_FLAG_EVICTION) ? " eviction" : "", segments, identity);
}

// One escape of `journal follow`, on the held adapter: the records from `from` into *journal. 0 on success, 1 when
// the adapter could not be opened or the escape failed (the held handle is closed then and the next call reopens
// it), 3 on a driver refusal; `report` prints the reason.
static int JournalPage(BC250_ESCAPE_PAGING_JOURNAL *journal, unsigned long long from, int report)
{
    NTSTATUS status;

    memset(journal, 0, sizeof(*journal));
    journal->Magic = BC250_ESCAPE_MAGIC;
    journal->Command = BC250_ESCAPE_GET_PAGING_JOURNAL;
    journal->From = from;
    if (SendEscapeHeld(BC250_DEFAULT_HWID, journal->Command, journal, sizeof(*journal), &status)) return 1;
    if (!NT_SUCCESS(status)) { if (report) PrintStatus("D3DKMTEscape", status); return 1; }
    if (journal->Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { if (report) printf("refused: caller is not an administrator\n"); return 3; }
    if (journal->Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) {
        if (report) printf("refused: this driver build has no paging journal (0.7.179 or older)\n");
        return 3;
    }
    if (journal->Status != BC250_ESCAPE_STATUS_DONE) {
        if (report) printf("refused: driver status %lu, NTSTATUS 0x%08lX %s\n", journal->Status, journal->NtStatus,
                           StatusName((NTSTATUS)journal->NtStatus));
        return 3;
    }
    return 0;
}

// journal follow SECONDS [MS]: one process, one adapter handle, one escape per interval, printing the records the
// KMD added since the previous read (the first read only positions the cursor at Next). The trial sampler that
// spawned a fresh bc250kmd_cli every second alongside the present heartbeat left the lab's sshd accepting
// nothing for as long as it ran (2026-09-30, trial 152 and scratch\dpm\test-shape.sh); a single long process
// like `dpm N MS` never did. A liveness line every 30 s, a summary at the end.
//
// 0.7.183.1: the adapter is opened once and held (SendEscapeHeld); up to 182 every read found and opened it
// again. A failed read no longer ends the run: the handle is dropped, the failure is printed once, and the next
// interval reopens. A journal whose total fell below the cursor belongs to a reloaded driver (the PnP restart
// of the GPU DWM ladder), and the cursor goes back to its oldest record.
//
// 0.7.184.1: the reads carry NoAdapterSynchronization alone (ReadEscapeOn), falling back to HardwareAccess against an
// older driver; the final line counts the escapes of each kind.
static int JournalFollow(const WCHAR *secondsText, const WCHAR *msText)
{
    static BC250_ESCAPE_PAGING_JOURNAL journal;
    unsigned long long from, printed = 0, lost = 0;
    unsigned long seconds, ms = 1000, reads = 0, failures = 0, failing = 0, restarts = 0;
    ULONGLONG start, lastLive;
    WCHAR *end;
    int rc;

    seconds = wcstoul(secondsText, &end, 10);
    if (*end || seconds == 0 || seconds > 86400) { fprintf(stderr, "journal follow SECONDS [MS]: SECONDS 1..86400\n"); return 2; }
    if (msText != NULL) {
        ms = wcstoul(msText, &end, 10);
        if (*end || ms < 50 || ms > 60000) { fprintf(stderr, "journal follow SECONDS [MS]: MS 50..60000\n"); return 2; }
    }
    rc = JournalPage(&journal, ~0ull, 1);
    if (rc) { ReleaseHeldAdapter(); return rc; }
    from = journal.Next;
    printf("journal follow: %lu s every %lu ms from record %llu (%llu written so far, ring of %lu, table %s)\n",
           seconds, ms, from, journal.Total, journal.Capacity,
           (journal.Flags & BC250_ESCAPE_FLAG_FULL_WDDM) ? "FULL WDDM" : "display-only");
    fflush(stdout);
    start = lastLive = GetTickCount64();
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now - start >= (ULONGLONG)seconds * 1000ull) break;
        rc = JournalPage(&journal, from, 0);
        if (rc == 3) {
            printf("journal follow: the driver refused read %lu (status %lu)\n", reads + 1, journal.Status);
            fflush(stdout);
            ReleaseHeldAdapter();
            return rc;
        }
        if (rc) {
            failures++;
            if (!failing++) printf("journal follow: t=%llu s read %lu failed, reopening the adapter at the next interval\n",
                                   (now - start) / 1000ull, reads + 1);
            fflush(stdout);
            Sleep(ms);
            continue;
        }
        if (failing) {
            printf("journal follow: t=%llu s reads resumed after %lu failed (adapter opens %lu)\n",
                   (now - start) / 1000ull, failing, g_HeldOpens);
            failing = 0;
        }
        if (journal.Total < from) {
            printf("journal follow: the journal holds %llu records, fewer than the cursor %llu: a reloaded driver; "
                   "following from its oldest record\n", journal.Total, from);
            restarts++;
            from = 0;
            continue;
        }
        reads++;
        lost += journal.Lost;
        for (unsigned long i = 0; i < journal.Returned && i < BC250_PAGING_JOURNAL_MAX; i++) PrintJournalRecord(&journal, i);
        printed += journal.Returned;
        if (journal.Next > from) from = journal.Next;
        if (journal.Returned == BC250_PAGING_JOURNAL_MAX) continue;   // a burst: drain it before sleeping
        if (now - lastLive >= 30000ull) {
            printf("journal follow: t=%llu s reads %lu printed %llu lost %llu next %llu\n",
                   (now - start) / 1000ull, reads, printed, lost, from);
            lastLive = now;
        }
        fflush(stdout);
        Sleep(ms);
    }
    printf("journal follow: done, %lu reads, %llu records printed, %llu lost to the ring, next %llu; "
           "%lu failed reads, %lu adapter opens, %lu driver reloads; escapes: %lu without adapter synchronization, "
           "%lu with HardwareAccess\n",
           reads, printed, lost, from, failures, g_HeldOpens, restarts, g_SoftReads, g_HardReads);
    ReleaseHeldAdapter();
    return 0;
}

static int Journal(const WCHAR *fromText)
{
    static BC250_ESCAPE_PAGING_JOURNAL journal;     // 4.6 KB: a static, like the log's
    unsigned long long from = 0, printed = 0;
    int first = 1;
    NTSTATUS status;
    WCHAR *end;

    if (fromText != NULL) {
        from = _wcstoui64(fromText, &end, 10);
        if (*end || fromText[0] == L'-') {
            fprintf(stderr, "journal [from], where from is a decimal record index, not %ls\n", fromText);
            return 2;
        }
    }
    for (;;) {
        memset(&journal, 0, sizeof(journal));
        journal.Magic = BC250_ESCAPE_MAGIC;
        journal.Command = BC250_ESCAPE_GET_PAGING_JOURNAL;
        journal.From = from;
        if (SendReadEscape(BC250_DEFAULT_HWID, journal.Command, &journal, sizeof(journal), &status)) return 1;
        if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
        if (journal.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
        if (journal.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND) {
            printf("refused: this driver build has no paging journal (0.7.179 or older)\n");
            return 3;
        }
        if (journal.Status != BC250_ESCAPE_STATUS_DONE) {
            printf("refused: driver status %lu, NTSTATUS 0x%08lX %s\n", journal.Status, journal.NtStatus,
                   StatusName((NTSTATUS)journal.NtStatus));
            return 3;
        }
        if (first)
            printf("journal      %llu records since this driver load, ring of %lu, %llu requested but overwritten; table %s\n",
                   journal.Total, journal.Capacity, journal.Lost,
                   (journal.Flags & BC250_ESCAPE_FLAG_FULL_WDDM) ? "FULL WDDM" : "display-only");
        first = 0;
        for (unsigned long i = 0; i < journal.Returned && i < BC250_PAGING_JOURNAL_MAX; i++) {
            PrintJournalRecord(&journal, i);
            printed++;
        }
        if (journal.Returned == 0 || journal.Next <= from) break;   // the end, or a driver that is not moving on
        from = journal.Next;
        if (printed > 4ull * journal.Capacity) {
            printf("             stopped at %llu records; ask again from %llu\n", printed, from);
            break;
        }
    }
    printf("             %llu records printed\n", printed);
    return 0;
}

// ---- dpm: the clock governor's telemetry (BC250_ESCAPE_RUN_DPM, docs/design/dpm.md) ------------------------------
//
// "dpm [count [interval ms]]" prints one line per sample: the level the governor committed, the SMU's readback,
// temperature, GFX busy share, what the load wants, the thermal cap, the ceiling and what holds the clock, then where
// the busy share came from (grbm: GUI_ACTIVE samples; submit: the ring's submit-to-fence time), the submit share
// and the SDMA0 (paging) not-idle share.
// "dpm confirm" clears the pending mark of a DPM start once the start is healthy (administrator).

static const char *const g_DpmReason[] = { "none", "not-requested", "invalid-setting", "unconfirmed", "unclean",
                                           "registry", "no-smu", "not-run", "smu-error" };
static const char *const g_DpmThrottle[] = { "none", "thermal-soft", "thermal-hard", "sensor", "max-setting",
                                             "stable", "smu", "fixed", "thermal-warm", "thermal-ramp", "idle",
                                             "thermal-zone" };

// ABI 3 first (0.7.215, the SMU metrics tail). BC250_DPM_ABI after a driver refused ABI 3, BC250_DPM_ABI_1 after
// one refused ABI 2 (0.7.207).
static unsigned long g_DpmAbi = BC250_DPM_ABI_3;

static int DpmQuery(BC250_ESCAPE_DPM_EX *x, unsigned long op, unsigned long long generation)
{
    BC250_ESCAPE_DPM *d = &x->Dpm;
    NTSTATUS status;
    unsigned size;
    for (;;) {
        memset(x, 0, sizeof(*x));
        size = g_DpmAbi == BC250_DPM_ABI_3 ? (unsigned)sizeof(*x) :
               g_DpmAbi == BC250_DPM_ABI ? (unsigned)sizeof(*d) : BC250_DPM_ABI1_SIZE;
        d->Magic = BC250_ESCAPE_MAGIC;
        d->Command = BC250_ESCAPE_RUN_DPM;
        d->AbiVersion = g_DpmAbi;
        d->Op = op;
        d->ExpectedGeneration = generation;
        if (SendEscapeFlags(BC250_DEFAULT_HWID, d, size, 1, &status)) return 1;
        if (status != (NTSTATUS)0xC000000Dl || g_DpmAbi == BC250_DPM_ABI_1) break;
        // STATUS_INVALID_PARAMETER for 248 bytes: a driver before 0.7.215, which takes ABI 2 and ABI 1. For 192
        // bytes: a driver before 0.7.207, which takes ABI 1 alone.
        g_DpmAbi = g_DpmAbi == BC250_DPM_ABI_3 ? BC250_DPM_ABI : BC250_DPM_ABI_1;
    }
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape(BC250_ESCAPE_RUN_DPM)", status); return 1; }
    return 0;
}

static const char *const g_DpmMetricsState[] = { "off", "waiting", "ok", "refused", "no table", "bad table" };

// The SMU metrics state, once under the header (0.7.215): where the power reading comes from, or why there is none.
static void DpmPrintMetrics(const BC250_ESCAPE_DPM_EX *x)
{
    const BC250_DPM_METRICS *m = &x->Metrics;
    if (x->Dpm.AbiVersion != BC250_DPM_ABI_3) {
        printf("smu metrics: n/a (a driver before 0x000700D7 answers RUN_DPM ABI %lu)\n", x->Dpm.AbiVersion);
        return;
    }
    printf("smu metrics: %s, %lu tables, %lu failures", m->MetricsState < ARRAYSIZE(g_DpmMetricsState) ?
           g_DpmMetricsState[m->MetricsState] : "?", m->MetricsReads, m->MetricsFailures);
    if (m->MetricsReads)
        printf(", last %lu ms ago: gfx %lu mV %lu MHz %.2f C, soc %lu mV %.2f C, throttler 0x%04lX", m->MetricsAgeMs,
               m->GfxMv, m->GfxMHz, m->GfxTemperatureCc / 100.0, m->SocMv, m->SocTemperatureCc / 100.0,
               m->ThrottlerStatus);
    if (m->MetricsState == BC250_DPM_METRICS_OFF) printf(" (EnableSmuMetrics 0)");
    printf("\n");
}

// The end of a sample line: the package power from the SMU's own table, or why there is no reading.
static void DpmPrintPower(const BC250_ESCAPE_DPM_EX *x)
{
    const BC250_DPM_METRICS *m = &x->Metrics;
    if (x->Dpm.AbiVersion != BC250_DPM_ABI_3) printf("  power n/a\n");
    else if (!(x->Dpm.Flags & BC250_DPM_FLAG_POWER)) printf("  power ?\n");
    else
        printf("  power %5.1f W avg %5.1f W (gfx %4.1f W soc %4.1f W)\n", m->SocketPowerMw / 1000.0,
               m->SocketPowerAvgMw / 1000.0, m->GfxPowerMw / 1000.0, m->SocPowerMw / 1000.0);
}

// One sample line without its end; DpmPrintPower ends it.
static void DpmPrint(const BC250_ESCAPE_DPM *d)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    printf("%02u:%02u:%02u.%03u %s%s %4lu MHz %4lu mV (SMU %4lu MHz VID %3lu%s) %5.1f C%s busy %5.1f%% avg %5.1f%% "
           "want %4lu cap %4lu max %4lu throttle %s%s%s%s%s  up %lu down %lu thermal %lu err %lu  src %s submit %5.1f%% sdma %5.1f%%",
           now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
           d->Mode == 1 ? "dpm" : "fixed", (d->Flags & BC250_DPM_FLAG_RUNNING) ? "" : "(stopped)",
           d->CurrentMHz, d->CurrentMv, d->ObservedMHz, d->ObservedVid,
           (d->Flags & BC250_DPM_FLAG_CLOCK) ? "" : " old",
           d->TemperatureMc / 1000.0, (d->Flags & BC250_DPM_FLAG_TEMPERATURE) ? "" : "?",
           d->BusyPermille / 10.0, d->BusyAvgPermille / 10.0, d->WantMHz, d->CapMHz, d->MaxMHz,
           d->Throttle < ARRAYSIZE(g_DpmThrottle) ? g_DpmThrottle[d->Throttle] : "?",
           (d->Flags & BC250_DPM_FLAG_PENDING) ? " pending" : "",
           (d->Flags & BC250_DPM_FLAG_CONFIRMED) ? " confirmed" : "",
           (d->Flags & BC250_DPM_FLAG_PAUSED) ? " paused" : "",
           (d->Flags & BC250_DPM_FLAG_SESSION) ? " session" : "",
           d->Raises, d->Lowers, d->ThermalEvents, d->Errors,
           (d->Flags & BC250_DPM_FLAG_HW_BUSY) ? "grbm" : "submit", d->SubmitBusyPermille / 10.0, d->SdmaBusyPermille / 10.0);
}

// The idle state, once under the header (0.7.207): the point in force, the window the GPU must be quiet for,
// and what the state did so far. A driver before 0.7.207 answers RUN_DPM ABI 1 and has no idle state; ABI 3
// (0.7.215) contains the ABI 2 fields.
static void DpmPrintIdle(const BC250_ESCAPE_DPM *d)
{
    if (d->AbiVersion != BC250_DPM_ABI && d->AbiVersion != BC250_DPM_ABI_3) {
        printf("idle: n/a (a driver before 0x000700CF answers RUN_DPM ABI 1)\n");
        return;
    }
    if (d->IdleMHz == 0) {
        // A start that does not govern never configures the state (the driver says so in its log), so name that
        // first: without it a fixed-lab start reads as a setting the owner has to look for.
        if (!(d->Flags & BC250_DPM_FLAG_GOVERNING))
            printf("idle: off (this start does not govern the clock)\n");
        else
            printf("idle: off (DpmIdleMHz 0, a refused setting, or a point the firmware refused; %lu refusals)\n",
                   d->IdleRefusals);
        return;
    }
    printf("idle: %lu MHz%s after %lu ms under %lu permille busy; entries %lu exits %lu refusals %lu, "
           "%llu ms at the point\n", d->IdleMHz, (d->Flags & BC250_DPM_FLAG_IDLE) ? " (now)" : "",
           d->IdleHoldMs, d->IdleBusyPermille, d->IdleEntries, d->IdleExits, d->IdleRefusals, d->IdleMs);
}

// The joint power arm (0.7.216.7), only on a start that runs it: two flag bits, no field of its own. The driver log's
// "dpm: telemetry joint" lines carry the rest (the cap, the reason, the counters).
#ifndef BC250_DPM_FLAG_JOINT
#define BC250_DPM_FLAG_JOINT 16384u
#define BC250_DPM_FLAG_JOINT_CAP 32768u
#endif
static void DpmPrintJoint(const BC250_ESCAPE_DPM *d)
{
    if (!(d->Flags & BC250_DPM_FLAG_JOINT)) return;
    printf("joint: on (DpmJointGovernor 1), CPU clock limit %s\n",
           (d->Flags & BC250_DPM_FLAG_JOINT_CAP) ? "lowered by the arm now" : "not touched now");
}

// ---- dpm tune / dpm floor: the governor's thresholds and a runtime floor (BC250_ESCAPE_RUN_DPM_TUNE, 0.7.185.1) ----
//
// "dpm tune <up> <target> <down> [hold ms]" sets the thresholds in permille (the hold stays as it is when omitted),
// "dpm tune reset" puts thresholds and floor back to the defaults, "dpm floor <MHz|off>" sets or clears the runtime
// floor, "dpm tune" prints what is in force. The driver checks every value (bc250_dpm_tune_check: ranges, order, the
// two invariants, the hold, the floor against the start's ceiling) and logs every change with its old and new values.
// Writes need an administrator and a running DPM start; nothing survives a device start. Like `dpm`, every request
// goes with NoAdapterSynchronization alone: the escape is software state, and the governor thread applies it.
//
// "dpm tune thermal <hot step ms> <soft delta mC|off> <soft step ms>" (0.7.197.1, ABI 2) sets the thermal cap's
// timing: at most one step down per hot step, and with a delta, one level back after the soft step held below
// 87 C - delta. Three more arguments (0.7.213, ABI 3) set the soft thermal zone on the same operation:
// "dpm tune thermal <hot step ms> <soft delta mC|off> <soft step ms> [<zone delta mC|off> <zone step ms>
// <zone lead ms>]". The zone steps the cap down from 87 C - zone delta, judged on the reading plus its own slope
// extrapolated over the lead, and "off" there leaves the hot cap at 87 C as the only thermal rule. Omitting the three
// keeps the zone as it is. The tool asks with the newest ABI and steps down to 2 and then 1 as a driver fails the size
// with STATUS_INVALID_PARAMETER; everything the older ABI has then works as before.

static const char *const g_TuneError[] = { "none", "range", "order", "lowering-invariant", "raise-invariant", "hold",
                                           "floor", "thermal", "zone" };
#define TUNE_ERRORS (sizeof(g_TuneError) / sizeof(g_TuneError[0]))

// The ABI the driver answered, stepped down 3 -> 2 -> 1 as it refuses a size. The numbers are consecutive by
// construction, so one decrement is the next-older layout.
static unsigned long g_TuneAbi = BC250_DPM_TUNE_ABI;

static unsigned TuneAbiSize(unsigned long abi)
{
    if (abi == BC250_DPM_TUNE_ABI) return (unsigned)sizeof(BC250_ESCAPE_DPM_TUNE);
    return abi == BC250_DPM_TUNE_ABI_2 ? BC250_DPM_TUNE_ABI2_SIZE : BC250_DPM_TUNE_ABI1_SIZE;
}

// One RUN_DPM_TUNE round trip. 0 when the driver answered (whatever Status says), 1 after reporting why not.
static int TuneQuery(BC250_ESCAPE_DPM_TUNE *t, unsigned long op, unsigned long long generation, int quiet)
{
    NTSTATUS status;
    unsigned size;
    for (;;) {
        if (g_TuneAbi == BC250_DPM_TUNE_ABI_1 && op == BC250_DPM_TUNE_OP_THERMAL) {
            if (!quiet) printf("# this driver predates 0.7.197.1 (RUN_DPM_TUNE ABI 1): no thermal timing to set\n");
            return 1;
        }
        size = TuneAbiSize(g_TuneAbi);
        if (size < sizeof(*t)) memset((unsigned char *)t + size, 0, sizeof(*t) - size);
        t->Magic = BC250_ESCAPE_MAGIC;
        t->Command = BC250_ESCAPE_RUN_DPM_TUNE;
        t->AbiVersion = g_TuneAbi;
        t->Op = op;
        t->ExpectedGeneration = generation;
        if (SendEscapeFlags(BC250_DEFAULT_HWID, t, size, 1, &status)) return 1;
        if (status != (NTSTATUS)0xC000000Dl || g_TuneAbi == BC250_DPM_TUNE_ABI_1) break;
        // STATUS_INVALID_PARAMETER for this size: a driver older than this ABI. 184 bytes predates 0.7.213, 152 bytes
        // predates 0.7.197.1, and 120 bytes is where the command itself began (0.7.185.1).
        g_TuneAbi--;
    }
    if (!NT_SUCCESS(status)) {
        // A driver before 0.7.185.1 has no such command and refuses NoAdapterSynchronization for it.
        if (!quiet) {
            PrintStatus("D3DKMTEscape(BC250_ESCAPE_RUN_DPM_TUNE)", status);
            printf("# a driver before 0.7.185.1 (0x000700B9) has no runtime tuning\n");
        }
        return 1;
    }
    return 0;
}

static int TuneRead(BC250_ESCAPE_DPM_TUNE *t, int quiet)
{
    memset(t, 0, sizeof(*t));
    if (TuneQuery(t, BC250_DPM_TUNE_OP_READ, 0, quiet)) return 1;
    if (t->Status != BC250_ESCAPE_STATUS_DONE) {
        if (!quiet) printf("dpm tune: read refused, status %lu NTSTATUS 0x%08lX\n", t->Status, t->NtStatus);
        return 1;
    }
    return 0;
}

static void TuneFloorText(char *text, size_t size, unsigned long mhz)
{
    if (mhz) _snprintf_s(text, size, _TRUNCATE, "%lu MHz", mhz);
    else _snprintf_s(text, size, _TRUNCATE, "off");
}

// "below 85.5 C for 3000 ms" or "off". The reference is BC250_DPM_TUNE_HOT_MC (bc250kmd_escape.h), which the driver
// asserts equal to BC250_DPM_HOT_MC, 87 C, fixed since 0.7.195.
static void TuneSoftText(char *text, size_t size, unsigned long deltaMc, unsigned long stepMs)
{
    if (deltaMc) {
        long below = BC250_DPM_TUNE_HOT_MC - (long)deltaMc;
        _snprintf_s(text, size, _TRUNCATE, "below %ld.%ld C for %lu ms", below / 1000l, (below % 1000l) / 100l, stepMs);
    } else _snprintf_s(text, size, _TRUNCATE, "off");
}

// "from 86.0 C per 3000 ms, lead 15000 ms" or "off" (0.7.213). The same 87 C reference as the soft release, because the
// escape carries both as a delta below it.
static void TuneZoneText(char *text, size_t size, unsigned long deltaMc, unsigned long stepMs, unsigned long leadMs)
{
    if (deltaMc) {
        long from = BC250_DPM_TUNE_HOT_MC - (long)deltaMc;
        _snprintf_s(text, size, _TRUNCATE, "from %ld.%ld C per %lu ms, lead %lu ms", from / 1000l,
                    (from % 1000l) / 100l, stepMs, leadMs);
    } else _snprintf_s(text, size, _TRUNCATE, "off");
}

// "up 900 target 800 down 650 permille, hold 200 ms (default), floor off (default)": the values in force, and where
// they come from.
static void TunePrintState(const BC250_ESCAPE_DPM_TUNE *t)
{
    char floor[32];
    TuneFloorText(floor, sizeof(floor), t->FloorMHz);
    printf("up %lu target %lu down %lu permille, hold %lu ms (%s), floor %s (%s)", t->UpPermille, t->TargetPermille,
           t->DownPermille, t->DownHoldMs, (t->Flags & BC250_DPM_TUNE_FLAG_THRESHOLDS) ? "runtime" : "default", floor,
           (t->Flags & BC250_DPM_TUNE_FLAG_FLOOR) ? "runtime" : "default");
    if (t->AbiVersion >= BC250_DPM_TUNE_ABI_2) {
        TuneSoftText(floor, sizeof(floor), t->SoftReleaseDeltaMc, t->SoftReleaseStepMs);
        printf(", hot step %lu ms, soft release %s (%s)", t->HotStepMs, floor,
               (t->Flags & BC250_DPM_TUNE_FLAG_THERMAL) ? "runtime" : "default");
    }
    if (t->AbiVersion >= BC250_DPM_TUNE_ABI) {
        char zone[64];
        TuneZoneText(zone, sizeof(zone), t->ZoneDeltaMc, t->ZoneStepMs, t->ZoneLeadMs);
        printf(", soft zone %s (%s)", zone, (t->Flags & BC250_DPM_TUNE_FLAG_ZONE) ? "runtime" : "default");
    }
}

static void TuneExplain(const BC250_ESCAPE_DPM_TUNE *t, unsigned long up)
{
    if (t->Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("  needs an elevated prompt\n"); return; }
    if (t->NtStatus == 0xC000022Dul) { printf("  STATUS_RETRY: the device restarted between the read and the write; run it again\n"); return; }
    if (t->NtStatus == 0xC0000184ul) {      // STATUS_INVALID_DEVICE_STATE (ntstatus.h, not in windows.h)
        printf("  no running DPM governor: a fixed-lab start (DpmMode 0), a stopped device or a governor that gave up\n");
        return;
    }
    switch (t->Error) {
    case 1: printf("  every threshold is 100..1000 permille\n"); break;
    case 2: printf("  the order is down < target < up\n"); break;
    case 3: printf("  invariant 1: (down + 1) x 1100 <= up x 1000, so a step down never lands at or above up; with up %lu, "
                   "down at most %lu\n", up, up * 10ul / 11ul >= 1 ? up * 10ul / 11ul - 1ul : 0ul); break;
    case 4: printf("  invariant 2: a raise from some clock would land below down; raise target or lower down\n"); break;
    case 5: printf("  the hold is 100..5000 ms\n"); break;
    case 6: printf("  the floor is a clock of the table (1000..2000 in 100 MHz steps) at or below this start's ceiling, %lu MHz\n",
                   t->MaxMHz); break;
    case 7: printf("  hot step 250..10000 ms, soft delta off or 500..4500 mC (between 82 and 87 C), soft step 2000..30000 ms\n");
            break;
    case 8: printf("  zone delta off or 500..4000 mC and at least 500 mC above the soft delta (so the zone steps the cap "
                   "down above where it raises it again), zone step 250..30000 ms, zone lead at most 60000 ms; a zone "
                   "needs a soft release above it\n");
            break;
    default: break;
    }
}

// After a write: the governor thread takes the values at its next 25 ms tick. Wait for it a little, then say so.
static void TuneWaitApplied(BC250_ESCAPE_DPM_TUNE *t)
{
    int i;
    for (i = 0; i < 8 && !(t->Flags & BC250_DPM_TUNE_FLAG_APPLIED); i++) {
        Sleep(50);
        if (TuneRead(t, 1)) break;
    }
    if (t->Flags & BC250_DPM_TUNE_FLAG_APPLIED) printf("governor: running serial %lu\n", t->Applied);
    else printf("governor: NOT running these yet (serial %lu, governor at %lu): paused, stopped or gave up?\n",
                t->Serial, t->Applied);
}

static int TuneWrite(unsigned long op, const BC250_ESCAPE_DPM_TUNE *in, const char *name)
{
    BC250_ESCAPE_DPM_TUNE before, t;
    char floorBefore[32], floorAfter[32];
    if (TuneRead(&before, 0)) return 1;
    t = *in;
    if (op == BC250_DPM_TUNE_OP_THRESHOLDS && t.DownHoldMs == 0) t.DownHoldMs = before.DownHoldMs;
    // A THERMAL write with no zone arguments keeps the zone as it is (0.7.213). ZoneStepMs is the marker because no
    // admissible zone step is 0, while ZoneDeltaMc 0 is the zone off and has to reach the driver.
    if (op == BC250_DPM_TUNE_OP_THERMAL && t.ZoneStepMs == 0) {
        t.ZoneDeltaMc = before.ZoneDeltaMc;
        t.ZoneStepMs = before.ZoneStepMs;
        t.ZoneLeadMs = before.ZoneLeadMs;
    } else if (op == BC250_DPM_TUNE_OP_THERMAL && before.AbiVersion < BC250_DPM_TUNE_ABI) {
        printf("# this driver predates 0.7.213 (RUN_DPM_TUNE ABI %lu): no soft zone to set\n", before.AbiVersion);
        return 1;
    }
    if (TuneQuery(&t, op, before.Generation, 0)) return 1;
    if (t.Status != BC250_ESCAPE_STATUS_DONE) {
        printf("%s: refused, status %lu NTSTATUS 0x%08lX, error %s; in force: ", name, t.Status, t.NtStatus,
               t.Error < TUNE_ERRORS ? g_TuneError[t.Error] : "?");
        TunePrintState(&before);
        printf("\n");
        TuneExplain(&t, in->UpPermille);
        return 1;
    }
    TuneFloorText(floorBefore, sizeof(floorBefore), before.FloorMHz);
    TuneFloorText(floorAfter, sizeof(floorAfter), t.FloorMHz);
    printf("%s: up %lu -> %lu, target %lu -> %lu, down %lu -> %lu permille, hold %lu -> %lu ms, floor %s -> %s "
           "(ceiling %lu MHz, serial %lu)\n", name, before.UpPermille, t.UpPermille, before.TargetPermille, t.TargetPermille,
           before.DownPermille, t.DownPermille, before.DownHoldMs, t.DownHoldMs, floorBefore, floorAfter, t.MaxMHz, t.Serial);
    if (t.AbiVersion >= BC250_DPM_TUNE_ABI_2) {
        TuneSoftText(floorBefore, sizeof(floorBefore), before.SoftReleaseDeltaMc, before.SoftReleaseStepMs);
        TuneSoftText(floorAfter, sizeof(floorAfter), t.SoftReleaseDeltaMc, t.SoftReleaseStepMs);
        printf("%s: hot step %lu -> %lu ms, soft release %s -> %s\n", name, before.HotStepMs, t.HotStepMs, floorBefore,
               floorAfter);
    }
    if (t.AbiVersion >= BC250_DPM_TUNE_ABI) {
        char zoneBefore[64], zoneAfter[64];
        TuneZoneText(zoneBefore, sizeof(zoneBefore), before.ZoneDeltaMc, before.ZoneStepMs, before.ZoneLeadMs);
        TuneZoneText(zoneAfter, sizeof(zoneAfter), t.ZoneDeltaMc, t.ZoneStepMs, t.ZoneLeadMs);
        printf("%s: soft zone %s -> %s\n", name, zoneBefore, zoneAfter);
    }
    printf("in force: ");
    TunePrintState(&t);
    printf("\n");
    TuneWaitApplied(&t);
    return 0;
}

static int ParseNumber(const WCHAR *text, unsigned long *value)
{
    WCHAR *end = NULL;
    if (text == NULL || !*text) return 1;
    *value = wcstoul(text, &end, 10);
    return end == NULL || *end != 0;
}

static int DpmTune(int argc, WCHAR **argv)
{
    BC250_ESCAPE_DPM_TUNE t;
    memset(&t, 0, sizeof(t));
    if (argc == 3) {
        if (TuneRead(&t, 0)) return 1;
        printf("driver 0x%08lX, mode %s, ceiling %lu MHz, generation %llu: ", t.Version, t.Mode == 1 ? "dpm" : "fixed",
               t.MaxMHz, t.Generation);
        TunePrintState(&t);
        printf("; defaults up %lu target %lu down %lu hold %lu ms; serial %lu, governor at %lu%s, floor ticks %llu\n",
               t.DefaultUpPermille, t.DefaultTargetPermille, t.DefaultDownPermille, t.DefaultDownHoldMs, t.Serial,
               t.Applied, (t.Flags & BC250_DPM_TUNE_FLAG_GOVERNING) ? "" : " (not governing)", t.FloorTicks);
        if (t.AbiVersion >= BC250_DPM_TUNE_ABI_2) {
            char soft[32];
            TuneSoftText(soft, sizeof(soft), t.DefaultSoftReleaseDeltaMc, t.DefaultSoftReleaseStepMs);
            printf("thermal defaults: hot step %lu ms, soft release %s (soft step %lu ms)\n", t.DefaultHotStepMs, soft,
                   t.DefaultSoftReleaseStepMs);
        } else printf("# RUN_DPM_TUNE ABI 1 (a driver before 0.7.197.1): no thermal timing\n");
        if (t.AbiVersion >= BC250_DPM_TUNE_ABI) {
            char zone[64];
            TuneZoneText(zone, sizeof(zone), t.DefaultZoneDeltaMc, t.DefaultZoneStepMs, t.DefaultZoneLeadMs);
            printf("soft zone defaults: %s, slope window %lu ms%s\n", zone, t.ZoneSlopeMs,
                   (t.Flags & BC250_DPM_TUNE_FLAG_ZONE_OFF) ? "; the zone is OFF for this start (DpmThermalZone 0)" : "");
        } else if (t.AbiVersion >= BC250_DPM_TUNE_ABI_2)
            printf("# RUN_DPM_TUNE ABI 2 (a driver before 0.7.213): no soft thermal zone\n");
        return 0;
    }
    if (argc == 4 && !_wcsicmp(argv[3], L"reset")) return TuneWrite(BC250_DPM_TUNE_OP_RESET, &t, "dpm tune reset");
    if (argc >= 4 && !_wcsicmp(argv[3], L"thermal")) {
        if ((argc != 7 && argc != 10) || ParseNumber(argv[4], &t.HotStepMs) ||
            (_wcsicmp(argv[5], L"off") && (ParseNumber(argv[5], &t.SoftReleaseDeltaMc) || !t.SoftReleaseDeltaMc)) ||
            ParseNumber(argv[6], &t.SoftReleaseStepMs) ||
            (argc == 10 &&
             ((_wcsicmp(argv[7], L"off") && (ParseNumber(argv[7], &t.ZoneDeltaMc) || !t.ZoneDeltaMc)) ||
              ParseNumber(argv[8], &t.ZoneStepMs) || !t.ZoneStepMs || ParseNumber(argv[9], &t.ZoneLeadMs)))) {
            fprintf(stderr, "usage: bc250kmd_cli dpm tune thermal <hot step ms> <soft delta mC|off> <soft step ms> "
                            "[<zone delta mC|off> <zone step ms> <zone lead ms>]\n");
            return 2;
        }
        if (!_wcsicmp(argv[5], L"off")) t.SoftReleaseDeltaMc = 0;
        if (argc == 10 && !_wcsicmp(argv[7], L"off")) t.ZoneDeltaMc = 0;
        return TuneWrite(BC250_DPM_TUNE_OP_THERMAL, &t, "dpm tune thermal");
    }
    if (argc == 6 || argc == 7) {
        if (ParseNumber(argv[3], &t.UpPermille) || ParseNumber(argv[4], &t.TargetPermille) ||
            ParseNumber(argv[5], &t.DownPermille) || (argc == 7 && (ParseNumber(argv[6], &t.DownHoldMs) || !t.DownHoldMs))) {
            fprintf(stderr, "dpm tune: <up> <target> <down> [hold ms] are decimal numbers (permille, ms)\n");
            return 2;
        }
        return TuneWrite(BC250_DPM_TUNE_OP_THRESHOLDS, &t, "dpm tune");
    }
    fprintf(stderr, "usage: bc250kmd_cli dpm tune [<up> <target> <down> [hold ms] | thermal <hot ms> <soft mC|off> "
                    "<soft ms> [<zone mC|off> <zone step ms> <zone lead ms>] | reset]\n");
    return 2;
}

static int DpmFloor(int argc, WCHAR **argv)
{
    BC250_ESCAPE_DPM_TUNE t;
    memset(&t, 0, sizeof(t));
    if (argc != 4 || (_wcsicmp(argv[3], L"off") && ParseNumber(argv[3], &t.FloorMHz))) {
        fprintf(stderr, "usage: bc250kmd_cli dpm floor <MHz|off>   (MHz: 1000..2000 in 100 MHz steps, at most DpmMaxMHz)\n");
        return 2;
    }
    if (!_wcsicmp(argv[3], L"off")) t.FloorMHz = 0;
    return TuneWrite(BC250_DPM_TUNE_OP_FLOOR, &t, "dpm floor");
}

// ---- dpm curve: the operator's V/F curve and its trial (BC250_ESCAPE_RUN_DPM_CURVE, 0.7.213) ---------------------
//
// "dpm curve" prints the three curves side by side (the table's own line, what is stored, what runs now) with the
// lowest voltage each level admits. "dpm curve set <mV> ..." puts a whole curve on trial, one value per level from
// 1000 MHz up; "dpm curve offset <mV>" takes that many millivolts off every level the band allows, which is the
// usual first experiment; "dpm curve preset mild|medium|deep" uses the GUI's own three steps. A trial reverts by
// itself after its window unless "dpm curve keep" lands inside it; "dpm curve cancel" ends it now and
// "dpm curve reset" deletes the stored curve and goes back to the table's line.
//
// The window and the revert belong to the driver: this tool can be killed at any moment and the curve still goes
// back. Nothing reaches the registry before a keep.
#define CURVE_ERRORS 7
static const char *const g_CurveError[CURVE_ERRORS] = {
    "none", "a value outside 820..1000 mV", "deeper than the band allows at that clock",
    "the voltage falls as the clock rises", "1000 MHz must stay at 820 mV", "no curve given",
    "the governor has not put the candidate into the chip yet"
};

static int CurveQuery(BC250_ESCAPE_DPM_CURVE *c, unsigned long op, unsigned long long generation, int quiet)
{
    NTSTATUS status;
    c->Magic = BC250_ESCAPE_MAGIC;
    c->Command = BC250_ESCAPE_RUN_DPM_CURVE;
    c->AbiVersion = BC250_DPM_CURVE_ABI;
    c->Op = op;
    c->ExpectedGeneration = generation;
    if (SendEscapeFlags(BC250_DEFAULT_HWID, c, sizeof(*c), 1, &status)) return 1;
    if (!NT_SUCCESS(status)) {
        if (!quiet) {
            PrintStatus("D3DKMTEscape(BC250_ESCAPE_RUN_DPM_CURVE)", status);
            printf("# a driver before 0.7.213 (0x000700D5) has no V/F curve\n");
        }
        return 1;
    }
    return 0;
}

static int CurveRead(BC250_ESCAPE_DPM_CURVE *c, int quiet)
{
    memset(c, 0, sizeof(*c));
    if (CurveQuery(c, BC250_DPM_CURVE_OP_READ, 0, quiet)) return 1;
    if (c->Status != BC250_ESCAPE_STATUS_DONE) {
        if (!quiet) printf("dpm curve: read refused, status %lu NTSTATUS 0x%08lX\n", c->Status, c->NtStatus);
        return 1;
    }
    return 0;
}

static void CurvePrint(const BC250_ESCAPE_DPM_CURVE *c)
{
    unsigned long i;
    printf("dpm curve: driver 0x%08lX, mode %s, ceiling %lu MHz, serial %lu applied %lu%s%s%s%s\n", c->Version,
           c->Mode == 1 ? "dpm" : "fixed", c->CeilingMHz, c->Serial, c->Applied,
           (c->Flags & BC250_DPM_CURVE_FLAG_GOVERNING) ? ", governing" : ", not governing",
           (c->Flags & BC250_DPM_CURVE_FLAG_STORED) ? ", a curve is stored" : "",
           (c->Flags & BC250_DPM_CURVE_FLAG_PENDING) ? ", PENDING (unconfirmed start)" : "",
           (c->Flags & BC250_DPM_CURVE_FLAG_CONFIRMED) ? ", confirmed" : "");
    if (c->Flags & BC250_DPM_CURVE_FLAG_ON_TRIAL)
        printf("dpm curve: ON TRIAL, %lu ms left (default window %lu ms); without a keep the stored curve comes back by itself\n",
               c->TrialRemainingMs, c->TrialMs);
    else
        printf("dpm curve: no trial runs; a set would get a %lu ms window\n", c->TrialMs);
    printf("   MHz   line  stored  active  trial   floor\n");
    for (i = 0; i < c->Points && i < BC250_DPM_CURVE_POINTS; i++) {
        unsigned long mhz = c->FirstMHz + i * c->StepMHz;
        printf("  %4lu  %5lu  %6lu  %6lu  %5lu  %6lu%s\n", mhz, c->DefaultMv[i], c->StoredMv[i], c->ActiveMv[i],
               c->CandidateMv[i], c->FloorMv[i], mhz == c->LevelMHz ? "   <- the governor is here" : "");
    }
    printf("dpm curve: now %lu MHz at %lu mV (SMU %lu MHz VID %lu), %ld.%01ld C; sets %lu keeps %lu cancels %lu "
           "reverts %lu\n", c->LevelMHz, c->LevelMv, c->ObservedMHz, c->ObservedVid, c->TemperatureMc / 1000,
           (c->TemperatureMc < 0 ? -c->TemperatureMc : c->TemperatureMc) % 1000 / 100, c->Sets, c->Keeps,
           c->Cancels, c->Reverts);
}

static int CurveWrite(unsigned long op, const unsigned long *mv, unsigned long windowMs, const char *name)
{
    BC250_ESCAPE_DPM_CURVE before, c;
    unsigned long i;
    if (CurveRead(&before, 0)) return 1;
    memset(&c, 0, sizeof(c));
    if (mv != NULL) for (i = 0; i < BC250_DPM_CURVE_POINTS; i++) c.CandidateMv[i] = mv[i];
    c.TrialMs = windowMs;
    if (CurveQuery(&c, op, before.Generation, 0)) return 1;
    if (c.Status != BC250_ESCAPE_STATUS_DONE) {
        printf("%s: refused, status %lu NTSTATUS 0x%08lX, error %s", name, c.Status, c.NtStatus,
               c.Error < CURVE_ERRORS ? g_CurveError[c.Error] : "?");
        if (c.Error) printf(" at %lu MHz", c.ErrorLevel ? c.FirstMHz + (c.ErrorLevel - 5) * 100 : c.FirstMHz);
        printf("\n");
        if (c.NtStatus == 0xC0000184ul)
            printf("# STATUS_INVALID_DEVICE_STATE: a curve needs a DPM start whose governor runs, and a keep or a "
                   "cancel needs a trial in flight (bc250kmd_cli dpm)\n");
        CurvePrint(&before);
        return 1;
    }
    printf("%s: done, serial %lu\n", name, c.Serial);
    CurvePrint(&c);
    return 0;
}

// The GUI's three steps, as millivolts off the table's line. The driver bounds the depth per level, so a preset
// that asks for more than a level allows is clamped to that level's floor here and not refused there.
static void CurveOffset(const BC250_ESCAPE_DPM_CURVE *c, unsigned long off, unsigned long *mv)
{
    unsigned long i;
    for (i = 0; i < BC250_DPM_CURVE_POINTS; i++) {
        unsigned long line = c->DefaultMv[i], floor = c->FloorMv[i];
        mv[i] = line > floor + off ? line - off : floor;
    }
}

static int DpmCurve(int argc, WCHAR **argv)
{
    BC250_ESCAPE_DPM_CURVE c;
    unsigned long mv[BC250_DPM_CURVE_POINTS], window = 0, value, i;
    if (argc == 3) {
        if (CurveRead(&c, 0)) return 1;
        CurvePrint(&c);
        return 0;
    }
    if (!_wcsicmp(argv[3], L"keep")) return CurveWrite(BC250_DPM_CURVE_OP_KEEP, NULL, 0, "dpm curve keep");
    if (!_wcsicmp(argv[3], L"cancel")) return CurveWrite(BC250_DPM_CURVE_OP_CANCEL, NULL, 0, "dpm curve cancel");
    if (!_wcsicmp(argv[3], L"reset")) return CurveWrite(BC250_DPM_CURVE_OP_RESET, NULL, 0, "dpm curve reset");
    if (!_wcsicmp(argv[3], L"offset") || !_wcsicmp(argv[3], L"preset")) {
        unsigned long off = 0;
        if (argc < 5) {
            fprintf(stderr, "usage: bc250kmd_cli dpm curve offset <mV> [window ms] | preset mild|medium|deep "
                            "[window ms]\n");
            return 2;
        }
        if (!_wcsicmp(argv[3], L"preset")) {
            if (!_wcsicmp(argv[4], L"mild")) off = 10;
            else if (!_wcsicmp(argv[4], L"medium")) off = 20;
            else if (!_wcsicmp(argv[4], L"deep")) off = 25;
            else { fprintf(stderr, "dpm curve preset: mild, medium or deep\n"); return 2; }
        } else if (ParseNumber(argv[4], &off)) {
            fprintf(stderr, "dpm curve offset: <mV> is a decimal number\n");
            return 2;
        }
        if (argc >= 6 && ParseNumber(argv[5], &window)) {
            fprintf(stderr, "dpm curve: [window ms] is a decimal number\n");
            return 2;
        }
        if (CurveRead(&c, 0)) return 1;
        CurveOffset(&c, off, mv);
        printf("dpm curve: %lu mV off the line where the band allows it\n", off);
        return CurveWrite(BC250_DPM_CURVE_OP_SET, mv, window, "dpm curve set");
    }
    if (!_wcsicmp(argv[3], L"set")) {
        if (argc != 4 + (int)BC250_DPM_CURVE_POINTS && argc != 5 + (int)BC250_DPM_CURVE_POINTS) {
            fprintf(stderr, "usage: bc250kmd_cli dpm curve set <mV at 1000> ... <mV at 2000> [window ms]   "
                            "(%lu values)\n", (unsigned long)BC250_DPM_CURVE_POINTS);
            return 2;
        }
        for (i = 0; i < BC250_DPM_CURVE_POINTS; i++) {
            if (ParseNumber(argv[4 + i], &value)) {
                fprintf(stderr, "dpm curve set: every value is a decimal number of millivolts\n");
                return 2;
            }
            mv[i] = value;
        }
        if (argc == 5 + (int)BC250_DPM_CURVE_POINTS && ParseNumber(argv[4 + BC250_DPM_CURVE_POINTS], &window)) {
            fprintf(stderr, "dpm curve set: [window ms] is a decimal number\n");
            return 2;
        }
        return CurveWrite(BC250_DPM_CURVE_OP_SET, mv, window, "dpm curve set");
    }
    fprintf(stderr, "usage: bc250kmd_cli dpm curve [set <mV>... | offset <mV> | preset mild|medium|deep | keep | "
                    "cancel | reset] [window ms]\n");
    return 2;
}

// ---- cpu: the clock limit, the undervolt, the temperature cap and the core mask (BC250_ESCAPE_RUN_CPU, 0.7.213) --
//
// "cpu" prints what is applied, stored and recorded and what the chip last answered; "cpu readback" sends the
// getters of both queues, which is also what admits every setter of this start. "cpu set [clock <MHz>]
// [uv <steps>] [temp <C>] [window <ms>]" puts a change on trial, "cpu keep" keeps it, "cpu cancel" ends it now and
// "cpu reset" puts the recorded baseline back and deletes the stored values. "cpu cores 6|8" writes the core mask,
// which the processor count follows after the next Windows restart. "cpu search [steps]" walks the undervolt one
// step at a time; the load between the steps is the caller's business, so the wrapper script runs it.
//
// The whole surface is off until CpuTune is 1 in the driver's Parameters key. Every write needs an administrator.
#define CPU_ERRORS 7
C_ASSERT(CPU_ERRORS == BC250_CPU_REQUEST_ERROR_COUNT);   // a new reason must not reach an operator as "?"
static const char *const g_CpuError[CPU_ERRORS] = {
    "none", "the clock is outside the admitted range", "the undervolt is deeper than the maximum",
    "the temperature cap is outside its band", "nothing to change", "the plan needs too many steps",
    "this start does not know the firmware's own boost ceiling, so a clock limit could not be given back"
};
#define CPU_FAILS 6
static const char *const g_CpuFail[CPU_FAILS] = {
    "none", "a machine check (WHEA)", "a wrong answer from the load", "clock stretching",
    "the voltage readback", "87 C"
};

static int CpuQuery(BC250_ESCAPE_CPU *c, unsigned long op, unsigned long long generation, int quiet)
{
    static int keepLegacy;          // a KEEP was refused without HardwareAccess: this driver is 0.7.212 or older
    BC250_ESCAPE_CPU sent;
    NTSTATUS status;
    c->Magic = BC250_ESCAPE_MAGIC;
    c->Command = BC250_ESCAPE_RUN_CPU;
    c->AbiVersion = BC250_CPU_ABI;
    c->Op = op;
    c->ExpectedGeneration = generation;
    sent = *c;                      // the request as built, for the retry below
    // READ is software state (NoAdapterSynchronization), and so is KEEP from 0.7.213: it writes the registry and
    // ends the trial without a mailbox message. Every other operation sends messages and takes the adapter,
    // exactly as a clock set does. A driver up to 0.7.212 refuses KEEP without HardwareAccess (Status REFUSED,
    // NtStatus STATUS_INVALID_PARAMETER): the request goes again with the old word, as in Bc250StartHealth, so that
    // a client of this release still keeps a trial against a driver that has not been restarted yet.
    if (op == BC250_CPU_OP_KEEP && keepLegacy) {
        if (SendEscapeFlags(BC250_DEFAULT_HWID, c, sizeof(*c), 0, &status)) return 1;
    } else {
        if (SendEscapeFlags(BC250_DEFAULT_HWID, c, sizeof(*c),
                            op == BC250_CPU_OP_READ || op == BC250_CPU_OP_KEEP, &status)) return 1;
        if (op == BC250_CPU_OP_KEEP && NT_SUCCESS(status) &&
            c->Status == BC250_ESCAPE_STATUS_REFUSED && c->NtStatus == 0xC000000Dul) {
            keepLegacy = 1;
            *c = sent;
            if (SendEscapeFlags(BC250_DEFAULT_HWID, c, sizeof(*c), 0, &status)) return 1;
        }
    }
    if (!NT_SUCCESS(status)) {
        if (!quiet) {
            PrintStatus("D3DKMTEscape(BC250_ESCAPE_RUN_CPU)", status);
            printf("# a driver before 0.7.213 (0x000700D5) has no CPU surface\n");
        }
        return 1;
    }
    return 0;
}

static int CpuRead(BC250_ESCAPE_CPU *c, int quiet)
{
    memset(c, 0, sizeof(*c));
    if (CpuQuery(c, BC250_CPU_OP_READ, 0, quiet)) return 1;
    if (c->Status != BC250_ESCAPE_STATUS_DONE) {
        if (!quiet) printf("cpu: read refused, status %lu NTSTATUS 0x%08lX\n", c->Status, c->NtStatus);
        return 1;
    }
    return 0;
}

static void CpuPrint(const BC250_ESCAPE_CPU *c)
{
    unsigned long i;
    printf("cpu: driver 0x%08lX, %s%s%s%s%s%s\n", c->Version,
           (c->Flags & BC250_CPU_FLAG_TUNE_ON) ? "on (CpuTune 1)" : "OFF (CpuTune is not 1: read-only)",
           (c->Flags & BC250_CPU_FLAG_QUEUE3_PROVEN) ? ", queue 3 answered" : ", queue 3 has not answered yet",
           (c->Flags & BC250_CPU_FLAG_STORED) ? ", values stored" : "",
           (c->Flags & BC250_CPU_FLAG_PENDING) ? ", PENDING (unconfirmed start)" : "",
           (c->Flags & BC250_CPU_FLAG_CONFIRMED) ? ", confirmed" : "",
           (c->Flags & BC250_CPU_FLAG_SEARCHING) ? ", a search runs" : "");
    if (c->Flags & BC250_CPU_FLAG_ON_TRIAL)
        printf("cpu: ON TRIAL, %lu ms left (default window %lu ms); without a keep the settings before it come back\n",
               c->TrialRemainingMs, c->TrialMs);
    printf("cpu: applied clock %lu MHz, undervolt %lu steps, cap %lu C (stored %lu / %lu / %lu, baseline %lu / %lu "
           "/ %lu)\n", c->AppliedMaxMHz, c->AppliedUvSteps, c->AppliedTempC, c->StoredMaxMHz, c->StoredUvSteps,
           c->StoredTempC, c->BaselineMaxMHz, c->BaselineUvSteps, c->BaselineTempC);
    printf("cpu: %lu mV (GPU %lu mV), firmware cap %lu C, features 0x%08lX, %ld.%01ld C\n", c->VoltageMv,
           c->GpuVoltageMv, c->CapC, c->Features, c->TemperatureMc / 1000,
           (c->TemperatureMc < 0 ? -c->TemperatureMc : c->TemperatureMc) % 1000 / 100);
    printf("cpu: cores");
    for (i = 0; i < BC250_CPU_CORE_SLOTS; i++) printf(" %lu", c->CoreMHz[i]);
    printf(" MHz; P-states");
    for (i = 0; i < BC250_CPU_CORE_SLOTS; i++) printf(" %lu", c->PstateMHz[i]);
    printf(" MHz\n");
    printf("cpu: mask 0x%02lX in force, 0x%02lX stored%s%s, %lu processors to Windows\n", c->CoreMask,
           c->CoreMaskStored, (c->Flags & BC250_CPU_FLAG_CORE_PENDING) ? ", PENDING" : "",
           (c->Flags & BC250_CPU_FLAG_CORE_CONFIRMED) ? ", confirmed" : "", c->Cores);
    if (c->SearchStep || c->SearchTested)
        printf("cpu: search step %lu, best %lu, tested %lu, stopped by %s\n", c->SearchStep, c->SearchBest,
               c->SearchTested, c->SearchFail < CPU_FAILS ? g_CpuFail[c->SearchFail] : "?");
    printf("cpu: reads %lu writes %lu refusals %lu reverts %lu (retries %lu, refused %lu); last queue %lu "
           "message 0x%02lX argument 0x%08lX status 0x%08lX\n", c->Reads, c->Writes, c->Refusals,
           c->Reverts, c->RevertRetries, c->RevertFailures, c->LastQueue, c->LastMessage, c->LastParameter,
           c->LastStatus);
    if (c->Flags & BC250_CPU_FLAG_REVERT_OWED)
        printf("cpu: A WAY BACK IS OWED: a revert was refused, so the trial settings are still in the chip. The "
               "driver repeats it every second; a cold start is the last backstop\n");
    if (!(c->Flags & BC250_CPU_FLAG_TEMP_VALID))
        printf("cpu: the temperature above was NOT read on the last message, so it is older than the rest\n");
    if ((c->Flags & BC250_CPU_FLAG_QUEUE3_PROVEN) && !(c->Flags & BC250_CPU_FLAG_BOOST_KNOWN))
        printf("cpu: the firmware's own boost ceiling has NOT answered in this start, so the clock limit is "
               "refused: a limit could only be given back as the P-state top, which is under the boost "
               "(BD-094). The undervolt and the temperature cap are unaffected\n");
}

// What the caller knows about the load it ran over this sample (0.7.211). The driver judges clock stretching
// only over a sample marked loaded, and a machine check is a sign no kernel reading of this surface carries, so
// a search driven from a script says both: `cpu search step loaded 1 whea 0`.
static int CpuSample(int argc, WCHAR **argv, int first, BC250_ESCAPE_CPU *c)
{
    int i;
    unsigned long value = 0;
    for (i = first; i < argc; i += 2) {
        if (i + 1 >= argc || ParseNumber(argv[i + 1], &value)) {
            fprintf(stderr, "usage: ... [whea <events>] [checksum <errors>] [loaded 0|1]\n");
            return 1;
        }
        if (!_wcsicmp(argv[i], L"whea")) c->WheaEvents = value;
        else if (!_wcsicmp(argv[i], L"checksum")) c->ChecksumErrors = value;
        else if (!_wcsicmp(argv[i], L"loaded")) c->Loaded = value;
        else {
            fprintf(stderr, "cpu search step: whea, checksum or loaded\n");
            return 1;
        }
    }
    return 0;
}

static int CpuWrite(unsigned long op, const BC250_ESCAPE_CPU *in, const char *name)
{
    BC250_ESCAPE_CPU before, c;
    if (CpuRead(&before, 0)) return 1;
    c = *in;
    if (CpuQuery(&c, op, before.Generation, 0)) return 1;
    if (c.Status != BC250_ESCAPE_STATUS_DONE) {
        printf("%s: refused, status %lu NTSTATUS 0x%08lX, error %s\n", name, c.Status, c.NtStatus,
               c.Error < CPU_ERRORS ? g_CpuError[c.Error] : "?");
        if (c.Error == BC250_CPU_REQUEST_ERROR_NO_CEILING)
            printf("# The clock control is refused for this start (BD-094): message 0x43 answered no clock inside "
                   "the band, and the boost probe did not make it answer either, so the way back out of the chip "
                   "could only be the P-state top, under the boost. A Windows restart is the way out; the "
                   "undervolt and the temperature cap still work\n");
        else if (c.NtStatus == 0xC0000184ul)
            printf("# STATUS_INVALID_DEVICE_STATE: CpuTune must be 1 in the driver's Parameters key, and a keep or "
                   "a cancel needs a trial in flight\n");
        if (c.NtStatus == 0xC00000A3ul)
            printf("# STATUS_DEVICE_NOT_READY: the read stage has not answered yet. Run `bc250kmd_cli cpu readback` "
                   "first; it is what admits every setter of this start\n");
        if (c.NtStatus == 0xC0000022ul)
            printf("# STATUS_ACCESS_DENIED: every operation of this surface, the readback included, needs an "
                   "elevated shell\n");
        CpuPrint(&before);
        return 1;
    }
    printf("%s: done, serial %lu\n", name, c.Serial);
    CpuPrint(&c);
    return 0;
}

static int Cpu(int argc, WCHAR **argv)
{
    BC250_ESCAPE_CPU c;
    unsigned long value = 0;
    int i;
    if (argc == 2) {
        if (CpuRead(&c, 0)) return 1;
        CpuPrint(&c);
        return 0;
    }
    memset(&c, 0, sizeof(c));
    if (!_wcsicmp(argv[2], L"readback")) return CpuWrite(BC250_CPU_OP_READBACK, &c, "cpu readback");
    if (!_wcsicmp(argv[2], L"keep")) return CpuWrite(BC250_CPU_OP_KEEP, &c, "cpu keep");
    if (!_wcsicmp(argv[2], L"cancel")) return CpuWrite(BC250_CPU_OP_CANCEL, &c, "cpu cancel");
    if (!_wcsicmp(argv[2], L"reset")) return CpuWrite(BC250_CPU_OP_RESET, &c, "cpu reset");
    if (!_wcsicmp(argv[2], L"cores")) {
        if (argc != 4 || ParseNumber(argv[3], &value) || (value != 6 && value != 8)) {
            fprintf(stderr, "usage: bc250kmd_cli cpu cores 6|8   (6 is the mask this part ships with)\n");
            return 2;
        }
        c.CoreMask = value == 8 ? BC250_CPU_REQUEST_MASK_FULL : BC250_CPU_REQUEST_MASK_STOCK;
        printf("cpu cores: mask 0x%02lX; the processor count follows after the next Windows restart\n", c.CoreMask);
        return CpuWrite(BC250_CPU_OP_CORES, &c, "cpu cores");
    }
    if (!_wcsicmp(argv[2], L"search")) {
        if (argc >= 4 && !_wcsicmp(argv[3], L"step")) {        // `cpu search step` reads as the hint prints it
            if (CpuSample(argc, argv, 4, &c)) return 2;
            return CpuWrite(BC250_CPU_OP_SEARCH_STEP, &c, "cpu search step");
        }
        if (argc >= 4 && (ParseNumber(argv[3], &value) || value > BC250_CPU_REQUEST_SEARCH_STEPS)) {
            fprintf(stderr, "usage: bc250kmd_cli cpu search [steps]   (at most %lu)\n",
                    (unsigned long)BC250_CPU_REQUEST_SEARCH_STEPS);
            return 2;
        }
        c.UvSteps = argc >= 4 ? value : 0;
        printf("# the load between the steps is the caller's: run `cpu search step loaded 1` after each load "
               "window\n");
        return CpuWrite(BC250_CPU_OP_SEARCH_BEGIN, &c, "cpu search");
    }
    if (!_wcsicmp(argv[2], L"step")) {
        if (CpuSample(argc, argv, 3, &c)) return 2;
        return CpuWrite(BC250_CPU_OP_SEARCH_STEP, &c, "cpu search step");
    }
    if (!_wcsicmp(argv[2], L"set")) {
        for (i = 3; i < argc; i += 2) {
            if (i + 1 >= argc || ParseNumber(argv[i + 1], &value)) {
                fprintf(stderr, "usage: bc250kmd_cli cpu set [clock <MHz>] [uv <steps>] [temp <C>] "
                                "[window <ms>] [whea <events>] [checksum <errors>] [loaded 0|1]\n");
                return 2;
            }
            if (!_wcsicmp(argv[i], L"clock")) { c.Given |= BC250_CPU_GIVEN_MAX; c.MaxMHz = value; }
            else if (!_wcsicmp(argv[i], L"uv")) { c.Given |= BC250_CPU_GIVEN_UV; c.UvSteps = value; }
            else if (!_wcsicmp(argv[i], L"temp")) { c.Given |= BC250_CPU_GIVEN_TEMP; c.TempC = value; }
            else if (!_wcsicmp(argv[i], L"window")) c.TrialMs = value;
            else if (!_wcsicmp(argv[i], L"whea")) c.WheaEvents = value;
            else if (!_wcsicmp(argv[i], L"checksum")) c.ChecksumErrors = value;
            else if (!_wcsicmp(argv[i], L"loaded")) c.Loaded = value;
            else {
                fprintf(stderr, "cpu set: clock, uv, temp, window, whea, checksum or loaded\n");
                return 2;
            }
        }
        if (!c.Given) {
            fprintf(stderr, "cpu set: give at least one of clock, uv and temp\n");
            return 2;
        }
        return CpuWrite(BC250_CPU_OP_SET, &c, "cpu set");
    }
    fprintf(stderr, "usage: bc250kmd_cli cpu [readback | set ... | keep | cancel | reset | cores 6|8 | search "
                    "[steps] | search step [whea N] [checksum N] [loaded 1]]\n");
    return 2;
}

static int Dpm(int argc, WCHAR **argv)
{
    BC250_ESCAPE_DPM_EX x;
    const BC250_ESCAPE_DPM *d = &x.Dpm;
    BC250_ESCAPE_DPM_TUNE t;
    unsigned long count = 1, interval = 1000, i;
    if (argc >= 3 && !_wcsicmp(argv[2], L"tune")) return DpmTune(argc, argv);
    if (argc >= 3 && !_wcsicmp(argv[2], L"floor")) return DpmFloor(argc, argv);
    if (argc >= 3 && !_wcsicmp(argv[2], L"curve")) return DpmCurve(argc, argv);
    if (argc > 4) { fprintf(stderr, "usage: bc250kmd_cli dpm [count [interval ms]] | confirm | tune ... | floor ... "
                                    "| curve ...\n"); return 2; }
    if (argc >= 3 && !_wcsicmp(argv[2], L"confirm")) {
        if (DpmQuery(&x, BC250_DPM_OP_READ, 0)) return 1;
        if (DpmQuery(&x, BC250_DPM_OP_CONFIRM, d->Generation)) return 1;
        printf("dpm confirm: status %lu NTSTATUS 0x%08lX, flags 0x%lX%s%s\n", d->Status, d->NtStatus, d->Flags,
               (d->Flags & BC250_DPM_FLAG_PENDING) ? " pending" : "", (d->Flags & BC250_DPM_FLAG_CONFIRMED) ? " confirmed" : "");
        return d->Status == BC250_ESCAPE_STATUS_DONE ? 0 : 1;
    }
    if (argc >= 3) count = wcstoul(argv[2], NULL, 0);
    if (argc >= 4) interval = wcstoul(argv[3], NULL, 0);
    if (count == 0) count = 1;
    for (i = 0; i < count; i++) {
        if (i) Sleep(interval);
        if (DpmQuery(&x, BC250_DPM_OP_READ, 0)) return 1;
        if (d->Status != BC250_ESCAPE_STATUS_DONE) {
            printf("dpm: refused, status %lu NTSTATUS 0x%08lX (driver version 0x%08lX)\n", d->Status, d->NtStatus, d->Version);
            return 1;
        }
        if (i == 0) {
            // The header keeps its old head (dpm-lib.ps1 parses it); the governor's thresholds and floor follow it.
            printf("driver 0x%08lX, requested %s, reason %s, generation %llu, ticks %llu, uptime %llu ms", d->Version,
                   d->Requested == 1 ? "dpm" : (d->Requested == 0 ? "fixed" : "invalid"),
                   d->Reason < 9 ? g_DpmReason[d->Reason] : "?", d->Generation, d->Ticks, d->UptimeMs);
            if (TuneRead(&t, 1) == 0) {
                printf("; ");
                TunePrintState(&t);
            } else printf("; tune n/a (driver before 0x000700B9)");
            printf("\n");
            DpmPrintIdle(d);
            DpmPrintJoint(d);
            DpmPrintMetrics(&x);
        }
        DpmPrint(d);
        DpmPrintPower(&x);
    }
    return 0;
}

// ---- interop: the GPU DWM interop switches (BC250_ESCAPE_RUN_INTEROP, docs/design/gpu-dwm-interop-switches.md) --
//
// One line of what the start decided: which switches were requested (EnableGpuPresentBlit "blit",
// EnableCddDwmInterop "cdd"), which are effective, the reason, and whether the driver closed them itself after an
// unclean boot; a second line with the session marker. Exit 0 when the escape answered, whatever the state.

static const char *const g_InteropReason[] = { "none", "not-requested", "invalid-setting", "unused", "unclean",
                                               "registry", "unused", "not-run" };
static const char *const g_InteropEnd[] = { "none", "device-stop", "last-user-gone", "system-power", "adapter-d3" };

static const char *InteropBits(unsigned long bits)
{
    static const char *const names[] = { "none", "blit", "cdd", "blit+cdd" };
    return names[bits & 3u];
}

static void InteropSetting(const char *name, unsigned long value, unsigned long flags, unsigned long absent,
                           unsigned long unreadable)
{
    if (flags & absent) printf(" %s=absent(1)", name);
    else if (flags & unreadable) printf(" %s=unreadable", name);
    else printf(" %s=%lu", name, value);
}

static int Interop(void)
{
    BC250_ESCAPE_INTEROP d;
    NTSTATUS status;
    memset(&d, 0, sizeof(d));
    d.Magic = BC250_ESCAPE_MAGIC;
    d.Command = BC250_ESCAPE_RUN_INTEROP;
    d.AbiVersion = BC250_INTEROP_ABI;
    d.Op = BC250_INTEROP_OP_READ;
    if (SendEscapeFlags(BC250_DEFAULT_HWID, &d, sizeof(d), 1, &status)) return 1;
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape(BC250_ESCAPE_RUN_INTEROP)", status); return 1; }
    if (d.Status != BC250_ESCAPE_STATUS_DONE) {
        printf("interop: refused, status %lu NTSTATUS 0x%08lX (driver version 0x%08lX)\n", d.Status, d.NtStatus, d.Version);
        return 1;
    }
    printf("driver 0x%08lX, requested %s, effective %s, reason %s%s, generation %llu\n", d.Version,
           InteropBits(d.Requested), InteropBits(d.Effective), d.Reason < 8 ? g_InteropReason[d.Reason] : "?",
           (d.Flags & BC250_INTEROP_FLAG_VALID) ? "" : " (no full WDDM start)", d.Generation);
    printf("settings");
    InteropSetting("EnableGpuPresentBlit", d.BlitSetting, d.Flags, BC250_INTEROP_FLAG_BLIT_ABSENT, BC250_INTEROP_FLAG_BLIT_UNREADABLE);
    InteropSetting("EnableCddDwmInterop", d.CddSetting, d.Flags, BC250_INTEROP_FLAG_CDD_ABSENT, BC250_INTEROP_FLAG_CDD_UNREADABLE);
    if (d.Flags & BC250_INTEROP_FLAG_CLOSED_BY_DRIVER)
        printf("  closed by the driver: %s", d.ClosedReason < 8 ? g_InteropReason[d.ClosedReason] : "?");
    printf("%s%s%s%s\n", (d.Flags & BC250_INTEROP_FLAG_UNCLEAN) ? "  last boot died in a session" : "",
           (d.Flags & BC250_INTEROP_FLAG_STALE) ? "  stale marker of this boot cleared" : "",
           (d.Flags & BC250_INTEROP_FLAG_PERSISTED) ? "  close written" : "",
           (d.Flags & BC250_INTEROP_FLAG_PERSIST_FAILED) ? "  close NOT written (retried next start)" : "");
    // 0.7.198 on: the power callback that ends a session at a clean restart (BD-059); older drivers set neither bit.
    if (d.Version >= 0x000700C6u)
        printf("power callback %s%s\n", (d.Flags & BC250_INTEROP_FLAG_POWER_CALLBACK) ? "registered" : "NOT registered",
               (d.Flags & BC250_INTEROP_FLAG_DOWN) ? ", system power transition under way (no mark)" : "");
    printf("session %s, users %lu, marks %lu, unmarks %lu, mark failures %lu, last end %s, previous end %s, "
           "boot %lu, marker found %lu\n",
           (d.Flags & BC250_INTEROP_FLAG_SESSION) ? "marked" : "not marked", d.Users, d.Marks, d.Unmarks, d.MarkFailures,
           d.LastEnd < 5 ? g_InteropEnd[d.LastEnd] : "?", d.PreviousEnd < 5 ? g_InteropEnd[d.PreviousEnd] : "?",
           d.BootId, d.SessionBootId);
    return 0;
}

// ---- ---------------------------------------------------------------------------------------------------------

// ---- dpaudio: DisplayPort audio, steps 0 to 2 (BC250_ESCAPE_RUN_DPAUDIO, driver/kmd/dpaudio.c) -------------------
//
// "bc250kmd_cli dpaudio" reads the step 0 registers now (OBSERVE) and prints the check table, the decision a start
// would take over them (the driver's own Bc250DpAudioDecide, not a copy of it here), the raw slots and the record
// of the last start. "bc250kmd_cli dpaudio state" prints the record alone and reads no register. Expected values
// are unit A's under Linux (facts M819, M820, evidence/linux/2026-10-07-L1007-dp-audio). The CLI asks with ABI 2
// (the stream record of step 2, KMD 0.7.216) and asks again with the ABI 1 prefix when an older driver refuses the
// size, so it reads 0.7.215 as well.

static const char *const g_DpAudioSlot[] = {
#define BC250_DPAUDIO_SLOT_NAME(n) #n,
    BC250_DPAUDIO_OBS_LIST(BC250_DPAUDIO_SLOT_NAME)
#undef BC250_DPAUDIO_SLOT_NAME
};
static const char *const g_DpAudioReason[] = {
#define BC250_DPAUDIO_REASON_NAME(n, t) t,
    BC250_DPAUDIO_REASON_LIST(BC250_DPAUDIO_REASON_NAME)
#undef BC250_DPAUDIO_REASON_NAME
};
static const char *const g_DpAudioState[] = { "idle", "enabled", "refused", "failed", "stopped", "path-off" };
static const char *const g_DpAudioStreamState[] = { "off", "on", "undone" };
static const char *const g_DpAudioStep[] = {
#define BC250_DPAUDIO_STEP_NAME(n) #n,
    BC250_DPAUDIO_STEP_LIST(BC250_DPAUDIO_STEP_NAME)
#undef BC250_DPAUDIO_STEP_NAME
};

static const char *DpAudioReasonText(unsigned long reason)
{
    return reason < BC250_DPAUDIO_REASON_COUNT ? g_DpAudioReason[reason] : "unknown reason";
}

static void DpAudioNotes(unsigned long notes, char *text, size_t size)
{
    _snprintf_s(text, size, _TRUNCATE, "%s%s%s%s%s", notes ? "" : "none",
                (notes & BC250_DPAUDIO_NOTE_HPD_LOW) ? " hpd-sense-low" : "",
                (notes & BC250_DPAUDIO_NOTE_INHERITED) ? " audio-enabled-inherited" : "",
                (notes & BC250_DPAUDIO_NOTE_REVISION) ? " codec-revision-differs" : "",
                (notes & BC250_DPAUDIO_NOTE_UNSOLICITED) ? " unsolicited-enabled" : "");
}

#define DPA(slot) d->Regs[BC250_DPAUDIO_OBS_##slot]
#define DPA_OK(slot) ((d->ValidMask >> BC250_DPAUDIO_OBS_##slot) & 1ull)
#define FIELD(v, mask, shift) (((v) & (mask)) >> (shift))

static void DpAudioCheck(const char *what, int valid, unsigned long value, const char *expected, int pass, const char *detail)
{
    if (!valid) { printf("  %-28s %-10s %-12s NOT READ\n", what, "-", expected); return; }
    printf("  %-28s 0x%08lX %-12s %-4s %s\n", what, value, expected, pass ? "ok" : "DIFF", detail);
}

static void DpAudioPrintObserve(const BC250_ESCAPE_DPAUDIO *d)
{
    char detail[160], notes[96];
    unsigned long n, read = 0;

    for (n = 0; n < BC250_DPAUDIO_OBS_COUNT; n++) read += (unsigned long)((d->ValidMask >> n) & 1ull);
    printf("check table (now / unit A under Linux, M819 M820):\n");
    DpAudioCheck("codec vendor/device", DPA_OK(CODEC_VENDOR_DEVICE), DPA(CODEC_VENDOR_DEVICE), "0x1002AA01",
                 DPA(CODEC_VENDOR_DEVICE) == 0x1002AA01ul, "start refuses on a difference");
    DpAudioCheck("codec revision", DPA_OK(CODEC_REVISION), DPA(CODEC_REVISION), "0x00100700",
                 DPA(CODEC_REVISION) == 0x00100700ul, "a note only");
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "DC_PINSTRAPS_AUDIO %lu (0 = no audio endpoint)",
                FIELD(DPA(DC_PINSTRAPS), DC_PINSTRAPS__DC_PINSTRAPS_AUDIO_MASK, DC_PINSTRAPS__DC_PINSTRAPS_AUDIO__SHIFT));
    DpAudioCheck("DC_PINSTRAPS", DPA_OK(DC_PINSTRAPS), DPA(DC_PINSTRAPS), "AUDIO != 0",
                 (DPA(DC_PINSTRAPS) & DC_PINSTRAPS__DC_PINSTRAPS_AUDIO_MASK) != 0, detail);
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "DP_VID_STREAM_ENABLE %lu",
                FIELD(DPA(DP0_VID_STREAM_CNTL), DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
                      DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE__SHIFT));
    DpAudioCheck("DP0_VID_STREAM_CNTL", DPA_OK(DP0_VID_STREAM_CNTL), DPA(DP0_VID_STREAM_CNTL), "enable 1",
                 (DPA(DP0_VID_STREAM_CNTL) & DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK) != 0, detail);
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "DP_VID_STREAM_ENABLE %lu",
                FIELD(DPA(DP1_VID_STREAM_CNTL), DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
                      DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE__SHIFT));
    DpAudioCheck("DP1_VID_STREAM_CNTL", DPA_OK(DP1_VID_STREAM_CNTL), DPA(DP1_VID_STREAM_CNTL), "enable 0",
                 (DPA(DP1_VID_STREAM_CNTL) & DP1_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK) == 0, detail);
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "FE_SOURCE_SELECT 0x%02lX, DIG_MODE %lu (0 = DP SST)",
                FIELD(DPA(DIG0_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK, DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT),
                FIELD(DPA(DIG0_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_MODE_MASK, DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT));
    DpAudioCheck("DIG0_BE_CNTL", DPA_OK(DIG0_BE_CNTL), DPA(DIG0_BE_CNTL), "FE 1 mode 0",
                 FIELD(DPA(DIG0_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK, DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT) == 1 &&
                 FIELD(DPA(DIG0_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_MODE_MASK, DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT) == 0, detail);
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "FE_SOURCE_SELECT 0x%02lX, DIG_MODE %lu",
                FIELD(DPA(DIG1_BE_CNTL), DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK, DIG1_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT),
                FIELD(DPA(DIG1_BE_CNTL), DIG1_DIG_BE_CNTL__DIG_MODE_MASK, DIG1_DIG_BE_CNTL__DIG_MODE__SHIFT));
    DpAudioCheck("DIG1_BE_CNTL", DPA_OK(DIG1_BE_CNTL), DPA(DIG1_BE_CNTL), "(unused)", 1, detail);
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "DC_HPD_SENSE %lu (a note only)",
                FIELD(DPA(HPD0_INT_STATUS), HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK, HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE__SHIFT));
    DpAudioCheck("HPD0_DC_HPD_INT_STATUS", DPA_OK(HPD0_INT_STATUS), DPA(HPD0_INT_STATUS), "sense 1",
                 (DPA(HPD0_INT_STATUS) & HPD0_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK) != 0, detail);
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "DC_HPD_SENSE %lu",
                FIELD(DPA(HPD1_INT_STATUS), HPD1_DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK, HPD1_DC_HPD_INT_STATUS__DC_HPD_SENSE__SHIFT));
    DpAudioCheck("HPD1_DC_HPD_INT_STATUS", DPA_OK(HPD1_INT_STATUS), DPA(HPD1_INT_STATUS), "(unused)", 1, detail);
    for (n = 0; n < 2; n++) {
        unsigned long ep = n ? BC250_DPAUDIO_OBS_EP1_CONFIG_DEFAULT : BC250_DPAUDIO_OBS_EP0_CONFIG_DEFAULT;
        unsigned long hp = n ? BC250_DPAUDIO_OBS_EP1_HOT_PLUG_CONTROL : BC250_DPAUDIO_OBS_EP0_HOT_PLUG_CONTROL;
        char what[40];

        _snprintf_s(what, sizeof(what), _TRUNCATE, "endpoint %lu CONFIG_DEFAULT", n);
        DpAudioCheck(what, (int)((d->ValidMask >> ep) & 1ull), d->Regs[ep], "0x185600F0", d->Regs[ep] == 0x185600F0ul,
                     "start refuses on a difference");
        _snprintf_s(what, sizeof(what), _TRUNCATE, "endpoint %lu HOT_PLUG_CONTROL", n);
        _snprintf_s(detail, sizeof(detail), _TRUNCATE, "AUDIO_ENABLED %lu, CLOCK_GATING_DISABLE %lu",
                    FIELD(d->Regs[hp], AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK,
                          AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED__SHIFT),
                    FIELD(d->Regs[hp], AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__CLOCK_GATING_DISABLE_MASK,
                          AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__CLOCK_GATING_DISABLE__SHIFT));
        DpAudioCheck(what, (int)((d->ValidMask >> hp) & 1ull), d->Regs[hp], "(state)", 1, detail);
    }
    printf("stream half (step 2 writes these; read only here):\n");
    for (n = 0; n < 2; n++) {
        unsigned long sec = n ? DPA(DP1_SEC_CNTL) : DPA(DP0_SEC_CNTL);
        unsigned long afmt = n ? DPA(DIG1_AFMT_CNTL) : DPA(DIG0_AFMT_CNTL);
        unsigned long pkt = n ? DPA(DIG1_AFMT_AUDIO_PACKET_CONTROL) : DPA(DIG0_AFMT_AUDIO_PACKET_CONTROL);
        printf("  DP%lu_SEC_CNTL 0x%08lX: stream %lu asp %lu atp %lu aip %lu; AUD_N 0x%08lX M_READBACK 0x%08lX\n", n, sec,
               FIELD(sec, DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK, DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE__SHIFT),
               FIELD(sec, DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK, DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE__SHIFT),
               FIELD(sec, DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK, DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE__SHIFT),
               FIELD(sec, DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK, DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE__SHIFT),
               n ? DPA(DP1_SEC_AUD_N) : DPA(DP0_SEC_AUD_N), n ? DPA(DP1_SEC_AUD_M_READBACK) : DPA(DP0_SEC_AUD_M_READBACK));
        printf("  DIG%lu_AFMT_CNTL 0x%08lX: audio clock en %lu on %lu; SRC_CONTROL 0x%08lX PACKET_CONTROL 0x%08lX"
               " (sample send %lu)\n", n, afmt,
               FIELD(afmt, DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK, DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN__SHIFT),
               FIELD(afmt, DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_ON_MASK, DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_ON__SHIFT),
               n ? DPA(DIG1_AFMT_AUDIO_SRC_CONTROL) : DPA(DIG0_AFMT_AUDIO_SRC_CONTROL), pkt,
               FIELD(pkt, DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK,
                     DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND__SHIFT));
    }
    printf("  DCCG_AUDIO_DTO_SOURCE 0x%08lX (DTO_SEL %lu); DTO0 %lu/%lu; DTO1 %lu/%lu\n",
           DPA(DTO_SOURCE), FIELD(DPA(DTO_SOURCE), DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK,
                                  DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL__SHIFT),
           DPA(DTO0_PHASE), DPA(DTO0_MODULE), DPA(DTO1_PHASE), DPA(DTO1_MODULE));
    // Step 2 sets the DTO1 module from this counter (100 kHz units, x 1000); Linux takes the clock manager's
    // spread-spectrum-adjusted figure instead, 5988740 on unit A, which the counter does not show (M788).
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, "%lu.%lu MHz, DTO1 module %lu", DPA(REFCLK_COUNT) / 10,
                DPA(REFCLK_COUNT) % 10, DPA(REFCLK_COUNT) * 1000ul);
    DpAudioCheck("CLK4_CLK2_CURRENT_CNT", DPA_OK(REFCLK_COUNT), DPA(REFCLK_COUNT), "5000..7000",
                 DPA(REFCLK_COUNT) >= 5000 && DPA(REFCLK_COUNT) <= 7000, detail);
    DpAudioNotes(d->ObsNotes, notes, sizeof(notes));
    printf("decision now (the driver's Bc250DpAudioDecide over these reads): %s; stream DP%lu, endpoint %lu, notes %s\n",
           DpAudioReasonText(d->ObsReason), d->ObsStream, d->ObsEndpoint, notes);
    printf("raw slots (%lu of %u read):\n", read, (unsigned)BC250_DPAUDIO_OBS_COUNT);
    for (n = 0; n < BC250_DPAUDIO_OBS_COUNT; n++) {
        if ((d->ValidMask >> n) & 1ull) printf("  R %-34s %08lX\n", g_DpAudioSlot[n], d->Regs[n]);
        else printf("  R %-34s not read\n", g_DpAudioSlot[n]);
    }
}

static void DpAudioPrintState(const BC250_ESCAPE_DPAUDIO *d, int abi2)
{
    char notes[96], sw[3][16];
    unsigned long i;
    const unsigned long sws[3] = { d->SwitchEnable, d->SwitchEndpoint, abi2 ? d->SwitchStream : BC250_DPAUDIO_NO_SWITCH };

    for (i = 0; i < 3; i++) {
        if (sws[i] == BC250_DPAUDIO_NO_SWITCH) strcpy_s(sw[i], sizeof(sw[i]), "not read yet");
        else _snprintf_s(sw[i], sizeof(sw[i]), _TRUNCATE, "%lu", sws[i]);
    }
    DpAudioNotes(d->Notes, notes, sizeof(notes));
    printf("record of the last start:\n");
    printf("  state %s, reason: %s\n", d->State < sizeof(g_DpAudioState) / sizeof(g_DpAudioState[0]) ?
           g_DpAudioState[d->State] : "?", DpAudioReasonText(d->Reason));
    printf("  stream DP%lu, endpoint %lu, notes %s\n", d->Stream, d->Endpoint, notes);
    if (abi2) printf("  switches EnableDpAudio %s, EnableDpAudioEndpoint %s, EnableDpAudioStream %s (default 1)\n",
                     sw[0], sw[1], sw[2]);
    else printf("  switches EnableDpAudio %s, EnableDpAudioEndpoint %s (default 1)\n", sw[0], sw[1]);
    printf("  codec 0x%08lX, config default 0x%08lX, HOT_PLUG_CONTROL 0x%08lX -> 0x%08lX, last NTSTATUS 0x%08lX\n",
           d->CodecId, d->ConfigDefault, d->HotPlugBefore, d->HotPlugAfter, d->LastStatus);
    printf("  starts %lu resumes %lu stops %lu refusals %lu failures %lu path-on %lu path-off %lu\n",
           d->Starts, d->Resumes, d->Stops, d->Refusals, d->Failures, d->PathOn, d->PathOff);
    printf("  accesses: indirect reads %lu, indirect writes %lu, direct writes %lu, refused by the tables %lu\n",
           d->IndirectReads, d->IndirectWrites, d->DirectWrites, d->AccessRefusals);
    if (!abi2) { printf("  stream: not reported (driver before 0.7.216, DP audio ABI 1)\n"); return; }
    printf("  stream %s, step %s, NTSTATUS 0x%08lX; on %lu off %lu undone %lu\n",
           d->StreamState < sizeof(g_DpAudioStreamState) / sizeof(g_DpAudioStreamState[0]) ?
           g_DpAudioStreamState[d->StreamState] : "?",
           d->StreamStep < BC250_DPAUDIO_STEP_COUNT ? g_DpAudioStep[d->StreamStep] : "?", (unsigned long)d->StreamStatus,
           d->StreamOn, d->StreamOff, d->StreamUndos);
    printf("  reference clock %lu (100 kHz units), DTO1 module %lu phase %lu, DTO_SOURCE 0x%08lX\n",
           d->RefClockCount, d->DtoModule, d->DtoPhase, d->DtoSource);
    printf("  read back: DP_SEC_CNTL 0x%08lX AFMT_CNTL 0x%08lX PACKET_CONTROL 0x%08lX PACKET_CONTROL2 0x%08lX\n",
           d->SecCntl, d->AfmtCntl, d->PacketControl, d->PacketControl2);
    if (d->MismatchOffset)
        printf("  mismatch at 0x%05lX: wrote 0x%08lX, read 0x%08lX\n", d->MismatchOffset, d->MismatchExpected,
               d->MismatchActual);
}

static int DpAudio(int argc, WCHAR **argv)
{
    static BC250_ESCAPE_DPAUDIO d;
    unsigned long op = BC250_DPAUDIO_OP_OBSERVE;
    NTSTATUS status = 0;
    int abi2;
    typedef char DpAudioAbiSizeCheck[(sizeof(BC250_ESCAPE_DPAUDIO) == 480 && BC250_DPAUDIO_ABI1_SIZE == 408) ? 1 : -1];
    (void)sizeof(DpAudioAbiSizeCheck);

    if (argc == 3 && !_wcsicmp(argv[2], L"state")) op = BC250_DPAUDIO_OP_STATE;
    else if (argc != 2) { fprintf(stderr, "usage: bc250kmd_cli dpaudio [state]\n"); return 2; }
    for (abi2 = 1; abi2 >= 0; abi2--) {
        memset(&d, 0, sizeof(d));
        d.Magic = BC250_ESCAPE_MAGIC;
        d.Command = BC250_ESCAPE_RUN_DPAUDIO;
        d.AbiVersion = abi2 ? BC250_DPAUDIO_ABI : BC250_DPAUDIO_ABI_1;
        d.Op = op;
        if (SendEscape(BC250_DEFAULT_HWID, &d, abi2 ? (unsigned)sizeof(d) : BC250_DPAUDIO_ABI1_SIZE, &status)) return 1;
        // A driver before 0.7.216 takes only the 408-byte ABI 1 record and refuses the size.
        if (!(abi2 && status == (NTSTATUS)0xC000000Dl)) break;
    }
    if (!NT_SUCCESS(status)) { PrintStatus("D3DKMTEscape", status); return 1; }
    if (d.Status == BC250_ESCAPE_STATUS_NOT_ADMIN) { printf("refused: caller is not an administrator\n"); return 3; }
    if (d.Status == BC250_ESCAPE_STATUS_UNKNOWN_COMMAND || d.Command != BC250_ESCAPE_RUN_DPAUDIO) {
        printf("refused: this driver build has no DP audio command\n");
        return 3;
    }
    printf("dpaudio %s: %s, NTSTATUS 0x%08lX %s, driver 0x%08lX, mmio %s\n", op == BC250_DPAUDIO_OP_STATE ? "state" : "observe",
           d.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", d.NtStatus, StatusName((NTSTATUS)d.NtStatus),
           d.Version, (d.Flags & BC250_ESCAPE_FLAG_MMIO_MAPPED) ? "mapped" : "not mapped (EnableMmio)");
    if (op == BC250_DPAUDIO_OP_OBSERVE && d.ValidMask) DpAudioPrintObserve(&d);
    if (abi2 && d.AbiVersion == BC250_DPAUDIO_ABI) DpAudioPrintState(&d, 1);
    else if (!abi2 && d.AbiVersion == BC250_DPAUDIO_ABI_1) DpAudioPrintState(&d, 0);
    return d.Status == BC250_ESCAPE_STATUS_DONE ? 0 : 3;
}
#undef DPA
#undef DPA_OK
#undef FIELD

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bc250kmd_cli info [hardware-id] | list | stages | confirm\n"
                        "       bc250kmd_cli health read | health confirm <generation> <epoch>\n"
                        "       bc250kmd_cli clock read | clock set <MHz> <mV>\n"
                        "       bc250kmd_cli telemetry [count [interval ms]]   (DPM snapshot and segment statistics)\n"
                        "       bc250kmd_cli vram [hardware-id]           (dxgkrnl segment statistics of any adapter)\n"
                        "       bc250kmd_cli budget <pid | image.exe> [hardware-id]   (one process: budget, usage, demoted)\n"
                        "       bc250kmd_cli read <hex offset | name> | write <hex offset> <hex value>\n"
                        "       bc250kmd_cli dpaudio [state]              (DP audio check table and record, dpaudio.c)\n"
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
                        "       bc250kmd_cli log [from] | log summary [from | only]   (only: the summary's own lines, for a poller)\n"
                        "       bc250kmd_cli dpm [count [interval ms]] | dpm confirm   (clock governor, docs/design/dpm.md)\n"
                        "       bc250kmd_cli dpm tune [<up> <target> <down> [hold ms] | reset] | dpm floor <MHz|off>\n"
                        "       bc250kmd_cli fan [count [interval ms]]    (the board's hardware monitor, docs/design/fan.md)\n"
                        "       bc250kmd_cli fan auto [store] | fan curve [standard|quiet|performance] [store]\n"
                        "       bc250kmd_cli fan set <percent> <seconds> | fan renew <seconds>    (admin; the fan control)\n"
                        "       bc250kmd_cli dpm curve [set <mV>... | offset <mV> | preset mild|medium|deep | keep | cancel | reset]\n"
                        "       bc250kmd_cli cpu [readback | set [clock <MHz>] [uv <steps>] [temp <C>] [window <ms>] | keep | cancel | reset]\n"
                        "       bc250kmd_cli cpu cores 6|8 | cpu search [steps] | cpu step   (docs/design/tuner.md)\n"
                        "       bc250kmd_cli interop                      (GPU DWM interop switches, docs/design/gpu-dwm-interop-switches.md)\n"
                        "       bc250kmd_cli journal [from]               (the paging journal, docs/design/paging-journal.md)\n"
                        "       bc250kmd_cli journal follow SECONDS [MS]  (one process printing new records every MS, default 1000)\n"
                        "       default hardware id: %ls\n", BC250_DEFAULT_HWID);
        return 2;
    }
    if (!_wcsicmp(argv[1], L"cpu")) return Cpu(argc,argv);
    if (!_wcsicmp(argv[1], L"health")) return StartHealth(argc,argv);
    if (!_wcsicmp(argv[1], L"clock")) return Clock(argc,argv);
    if (!_wcsicmp(argv[1], L"telemetry") && argc <= 4) return Telemetry(argc, argv);
    if (!_wcsicmp(argv[1], L"vram") && argc <= 3) return VideoMemory(argc == 3 ? argv[2] : NULL);
    if (!_wcsicmp(argv[1], L"budget") && (argc == 3 || argc == 4)) return Budget(argv[2], argc == 4 ? argv[3] : NULL);
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
    if (!_wcsicmp(argv[1], L"dpaudio") && argc <= 3) return DpAudio(argc, argv);
    if (!_wcsicmp(argv[1], L"dcnflip") && argc == 3 && !_wcsicmp(argv[2], L"restore")) return DcnFlip(NULL, NULL, NULL, 1);
    if (!_wcsicmp(argv[1], L"dcnflip") && argc == 3) return DcnFlip(argv[2], NULL, NULL, 0);
    if (!_wcsicmp(argv[1], L"dcnflip") && argc == 5) return DcnFlip(argv[2], argv[3], argv[4], 0);
    if (!_wcsicmp(argv[1], L"fence") && argc >= 3 && argc <= 5) return Fence(argc, argv);
    if (!_wcsicmp(argv[1], L"ib") && argc == 6) return Ib(argv);
    if (!_wcsicmp(argv[1], L"sdmacopy") && argc <= 3) return SdmaCopy(argc == 3 ? argv[2] : NULL, 0);
    if (!_wcsicmp(argv[1], L"sdmaib") && argc <= 3) return SdmaCopy(argc == 3 ? argv[2] : NULL, 1);
    if (!_wcsicmp(argv[1], L"fbdump") && argc == 3) return Fbdump(argv[2]);
    if (!_wcsicmp(argv[1], L"dpm") && argc <= 7) return Dpm(argc, argv);
    if (!_wcsicmp(argv[1], L"fan") && argc <= 5) return Fan(argc, argv);
    if (!_wcsicmp(argv[1], L"interop") && argc == 2) return Interop();
    if (!_wcsicmp(argv[1], L"journal") && argc >= 4 && argc <= 5 && !_wcsicmp(argv[2], L"follow"))
        return JournalFollow(argv[3], argc == 5 ? argv[4] : NULL);
    if (!_wcsicmp(argv[1], L"journal") && argc <= 3) return Journal(argc == 3 ? argv[2] : NULL);
    if (!_wcsicmp(argv[1], L"log") && argc <= 4) {
        if (argc >= 3 && !_wcsicmp(argv[2], L"summary")) return Log(argc == 4 ? argv[3] : NULL, 1);
        if (argc <= 3) return Log(argc == 3 ? argv[2] : NULL, 0);
    }
    fprintf(stderr, "unknown command %ls\n", argv[1]);
    return 2;
}
