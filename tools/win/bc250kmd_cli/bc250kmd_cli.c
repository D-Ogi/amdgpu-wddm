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
    printf("mode         %lux%lu pitch %lu format %lu\n", data.Width, data.Height, data.Pitch, data.ColorFormat);
    printf("presents     %lu\n", data.Presents);
    return 0;
}

// ---- read / write: registers through the escape (ADR 0007) ----------------------------------------------------
//
// Offsets are BAR5 byte offsets and come from tools/regcalc (on the target: bc250rd's reglist.txt), never from
// memory. The driver checks them against its own generated tables, so a wrong one is refused, not executed.

static int SendEscape(const WCHAR *wantedId, void *data, unsigned size, NTSTATUS *result)
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
    escape.Flags.HardwareAccess = 1;        // dxgkrnl then serializes the call with the rest of the adapter's work
    escape.pPrivateDriverData = data;
    escape.PrivateDriverDataSize = size;
    *result = D3DKMTEscape(&escape);

    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
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
                        KEY_SET_VALUE, NULL, &key, &disposition);
    if (s != ERROR_SUCCESS) {
        fprintf(stderr, "cannot open HKLM\\%ls, error %lu%s\n", BC250_PARAMETERS, (unsigned long)s,
                s == ERROR_ACCESS_DENIED ? " (is this an elevated prompt?)" : "");
        return 1;
    }
    s = RegSetValueExW(key, L"UnconfirmedStarts", 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
    RegCloseKey(key);
    if (s != ERROR_SUCCESS) {
        fprintf(stderr, "cannot write UnconfirmedStarts, error %lu\n", (unsigned long)s);
        return 1;
    }
    printf("UnconfirmedStarts = 0\n");
    return 0;
}

// ---- ---------------------------------------------------------------------------------------------------------

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bc250kmd_cli info [hardware-id] | list | stages | confirm\n"
                        "       bc250kmd_cli read <hex offset> | write <hex offset> <hex value>\n"
                        "       bc250kmd_cli memory | vread <phys|bar0> <hex offset> | vwrite <phys|bar0> <hex offset> <hex value>\n"
                        "       bc250kmd_cli vcompare <hex offset> <count>\n"
                        "       bc250kmd_cli gart plan | enable | restore\n"
                        "       default hardware id: %ls\n", BC250_DEFAULT_HWID);
        return 2;
    }
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
    if (!_wcsicmp(argv[1], L"gart") && argc == 3) return Gart(argv[2]);
    fprintf(stderr, "unknown command %ls\n", argv[1]);
    return 2;
}
