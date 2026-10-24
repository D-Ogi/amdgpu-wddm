// Discovery for the open ADL subset: which adapters run our kernel-mode driver, and what the operating system says
// about them. Every value comes from the system, nothing from a table:
//
//   identity      SetupAPI display-class device nodes whose service is our KMD (kOurServices). PCI ids from the
//                 first hardware id, name, driver key and DriverVersion from the device node.
//   running       D3DKMTEnumAdapters2: a device node counts only when a WDDM adapter with the same PCI location
//                 (KMTQAITYPE_ADAPTERADDRESS) exists, that is when our driver runs on it now. The LUID and the
//                 segment sizes (KMTQAITYPE_GETSEGMENTSIZE) come from that adapter.
//   sources       EnumDisplayDevices plus D3DKMTOpenAdapterFromGdiDisplayName: each \\.\DISPLAYn of our LUID is one
//                 ADL adapter, as with AMD's library. A GPU without a source gets one placeholder entry.
//   displays      QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS) on our LUID: monitor name, connector, position, size,
//                 rotation and refresh rate of each active path.
//
// No board-specific value lives here: the library is for every adapter that our driver runs on.
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <d3dkmthk.h>
#include <string.h>
#include <wchar.h>

#include "adl-abi.h"
#include "adl-model.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace {

// The service names of our kernel-mode driver (driver/kmd/bc250kmd.inf, AddService). A test build may name a host
// adapter's service instead, so that the discovery runs against real hardware without an AMD GPU.
#if defined(BC250_ADL_TEST_SERVICE)
#define ADL_WIDE_NAME2(x) L## #x
#define ADL_WIDE_NAME(x) ADL_WIDE_NAME2(x)
const wchar_t* const kOurServices[] = {ADL_WIDE_NAME(BC250_ADL_TEST_SERVICE)};
#else
const wchar_t* const kOurServices[] = {L"bc250kmd"};
#endif

struct Candidate {
    AdlGpu gpu;
    int matched;
};

void ToText(char* dst, size_t cap, const wchar_t* src)
{
    dst[0] = '\0';
    if (!src || !src[0]) {
        return;
    }
    int n = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, (int)cap, nullptr, nullptr);
    if (n <= 0) {
        // Too long for the field: convert what fits, the field stays terminated.
        n = WideCharToMultiByte(CP_ACP, 0, src, (int)wcsnlen(src, cap - 1), dst, (int)cap - 1, nullptr, nullptr);
        dst[n > 0 ? n : 0] = '\0';
    }
}

bool IsOurService(const wchar_t* service)
{
    for (const wchar_t* ours : kOurServices) {
        if (_wcsicmp(service, ours) == 0) {
            return true;
        }
    }
    return false;
}

bool GetStringProperty(HDEVINFO set, SP_DEVINFO_DATA* node, DWORD property, wchar_t* out, DWORD chars)
{
    DWORD type = 0;
    out[0] = L'\0';
    if (!SetupDiGetDeviceRegistryPropertyW(set, node, property, &type, (BYTE*)out, (chars - 1) * sizeof(wchar_t),
                                           nullptr)) {
        return false;
    }
    out[chars - 1] = L'\0';
    return type == REG_SZ || type == REG_MULTI_SZ;
}

bool GetDwordProperty(HDEVINFO set, SP_DEVINFO_DATA* node, DWORD property, DWORD* out)
{
    DWORD type = 0;
    return SetupDiGetDeviceRegistryPropertyW(set, node, property, &type, (BYTE*)out, sizeof(*out), nullptr) &&
           type == REG_DWORD;
}

// The hexadecimal value after "<key>" in a PnP id, for example VEN_ in PCI\VEN_1002&DEV_13FE&SUBSYS_...&REV_00.
uint32_t PnpField(const wchar_t* id, const wchar_t* key, int digits)
{
    const wchar_t* at = wcsstr(id, key);
    if (!at) {
        return 0;
    }
    at += wcslen(key);
    uint32_t value = 0;
    for (int i = 0; i < digits; i++) {
        wchar_t c = at[i];
        uint32_t d;
        if (c >= L'0' && c <= L'9') {
            d = (uint32_t)(c - L'0');
        } else if (c >= L'A' && c <= L'F') {
            d = (uint32_t)(c - L'A' + 10);
        } else if (c >= L'a' && c <= L'f') {
            d = (uint32_t)(c - L'a' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | d;
    }
    return value;
}

int FindCandidates(Candidate* out, int cap)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int count = 0;
    SP_DEVINFO_DATA node;
    node.cbSize = sizeof(node);
    for (DWORD i = 0; count < cap && SetupDiEnumDeviceInfo(set, i, &node); i++) {
        wchar_t text[512];
        if (!GetStringProperty(set, &node, SPDRP_SERVICE, text, 512) || !IsOurService(text)) {
            continue;
        }
        DWORD bus = 0, address = 0;
        if (!GetDwordProperty(set, &node, SPDRP_BUSNUMBER, &bus) ||
            !GetDwordProperty(set, &node, SPDRP_ADDRESS, &address)) {
            continue;
        }
        Candidate* c = &out[count];
        memset(c, 0, sizeof(*c));
        c->gpu.bus = bus;
        c->gpu.device = (address >> 16) & 0xffff;
        c->gpu.function = address & 0xffff;
        if (GetStringProperty(set, &node, SPDRP_HARDWAREID, text, 512)) {
            c->gpu.vendor_id = PnpField(text, L"VEN_", 4);
            c->gpu.device_id = PnpField(text, L"DEV_", 4);
            c->gpu.subsys_id = PnpField(text, L"SUBSYS_", 8);
            c->gpu.revision_id = PnpField(text, L"REV_", 2);
        }
        if (SetupDiGetDeviceInstanceIdW(set, &node, text, 512, nullptr)) {
            ToText(c->gpu.instance_id, sizeof(c->gpu.instance_id), text);
        }
        if (GetStringProperty(set, &node, SPDRP_FRIENDLYNAME, text, 512) ||
            GetStringProperty(set, &node, SPDRP_DEVICEDESC, text, 512)) {
            ToText(c->gpu.name, sizeof(c->gpu.name), text);
        }
        if (GetStringProperty(set, &node, SPDRP_DRIVER, text, 512)) {
            ToText(c->gpu.driver_key, sizeof(c->gpu.driver_key), text);
        }
        HKEY key = SetupDiOpenDevRegKey(set, &node, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
        if (key != INVALID_HANDLE_VALUE) {
            DWORD type = 0, bytes = sizeof(text) - sizeof(wchar_t);
            if (RegQueryValueExW(key, L"DriverVersion", nullptr, &type, (BYTE*)text, &bytes) == ERROR_SUCCESS &&
                type == REG_SZ) {
                text[bytes / sizeof(wchar_t)] = L'\0';
                ToText(c->gpu.driver_version, sizeof(c->gpu.driver_version), text);
            }
            RegCloseKey(key);
        }
        count++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

void CloseKmtAdapter(D3DKMT_HANDLE adapter)
{
    D3DKMT_CLOSEADAPTER close = {};
    close.hAdapter = adapter;
    D3DKMTCloseAdapter(&close);
}

bool QueryKmt(D3DKMT_HANDLE adapter, KMTQUERYADAPTERINFOTYPE type, void* data, UINT bytes)
{
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = adapter;
    query.Type = type;
    query.pPrivateDriverData = data;
    query.PrivateDriverDataSize = bytes;
    return NT_SUCCESS(D3DKMTQueryAdapterInfo(&query));
}

// Gives each candidate the LUID and memory sizes of the running WDDM adapter at its PCI location.
void MatchRunningAdapters(Candidate* candidates, int count)
{
    D3DKMT_ENUMADAPTERS2 enumerate = {};
    if (!NT_SUCCESS(D3DKMTEnumAdapters2(&enumerate)) || enumerate.NumAdapters == 0) {
        return;
    }
    // The list can grow between the two calls; ask for a few more.
    ULONG cap = enumerate.NumAdapters + 4;
    D3DKMT_ADAPTERINFO* list =
        (D3DKMT_ADAPTERINFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap * sizeof(D3DKMT_ADAPTERINFO));
    if (!list) {
        return;
    }
    enumerate.NumAdapters = cap;
    enumerate.pAdapters = list;
    if (NT_SUCCESS(D3DKMTEnumAdapters2(&enumerate))) {
        for (ULONG i = 0; i < enumerate.NumAdapters; i++) {
            D3DKMT_ADAPTERADDRESS address = {};
            if (QueryKmt(list[i].hAdapter, KMTQAITYPE_ADAPTERADDRESS, &address, sizeof(address))) {
                for (int c = 0; c < count; c++) {
                    AdlGpu* gpu = &candidates[c].gpu;
                    if (candidates[c].matched || gpu->bus != address.BusNumber ||
                        gpu->device != address.DeviceNumber || gpu->function != address.FunctionNumber) {
                        continue;
                    }
                    candidates[c].matched = 1;
                    gpu->luid_low = list[i].AdapterLuid.LowPart;
                    gpu->luid_high = list[i].AdapterLuid.HighPart;
                    D3DKMT_SEGMENTSIZEINFO sizes = {};
                    if (QueryKmt(list[i].hAdapter, KMTQAITYPE_GETSEGMENTSIZE, &sizes, sizeof(sizes))) {
                        gpu->dedicated_video_bytes = sizes.DedicatedVideoMemorySize;
                        gpu->dedicated_system_bytes = sizes.DedicatedSystemMemorySize;
                        gpu->shared_system_bytes = sizes.SharedSystemMemorySize;
                    }
                    break;
                }
            }
            CloseKmtAdapter(list[i].hAdapter);
        }
    }
    HeapFree(GetProcessHeap(), 0, list);
}

int FindGpu(const AdlSnapshot* s, LUID luid)
{
    for (int g = 0; g < s->gpu_count; g++) {
        if (s->gpus[g].luid_low == luid.LowPart && s->gpus[g].luid_high == luid.HighPart) {
            return g;
        }
    }
    return -1;
}

void AddEntries(AdlSnapshot* s)
{
    // Entries are grouped by GPU, in the order of the GPUs.
    for (int g = 0; g < s->gpu_count; g++) {
        int first = s->entry_count;
        DISPLAY_DEVICEW dd;
        for (DWORD i = 0; s->entry_count < ADL_MODEL_MAX_ENTRIES; i++) {
            memset(&dd, 0, sizeof(dd));
            dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) {
                break;
            }
            D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {};
            wcsncpy_s(open.DeviceName, dd.DeviceName, _TRUNCATE);
            if (!NT_SUCCESS(D3DKMTOpenAdapterFromGdiDisplayName(&open))) {
                continue;
            }
            CloseKmtAdapter(open.hAdapter);
            if (FindGpu(s, open.AdapterLuid) != g) {
                continue;
            }
            AdlEntry* e = &s->entries[s->entry_count++];
            e->gpu = g;
            e->present = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ? 1 : 0;
            e->os_display_index = (int32_t)i;
            e->source_id = (int32_t)open.VidPnSourceId;
            ToText(e->gdi_name, sizeof(e->gdi_name), dd.DeviceName);
        }
        if (s->entry_count == first && s->entry_count < ADL_MODEL_MAX_ENTRIES) {
            AdlEntry* e = &s->entries[s->entry_count++];
            e->gpu = g;
            e->present = 0;
            e->os_display_index = -1;
            e->source_id = -1;
        }
    }
}

void DescribeOutput(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech, AdlDisplay* d)
{
    d->display_type = ADL_DT_DIGITAL_FLAT_PANEL;
    d->output_type = ADL_DOT_DIGITAL;
    d->connector = ADL_DISPLAY_CONTYPE_UNKNOWN;
    switch (tech) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
        d->display_type = ADL_DT_MONITOR;
        d->output_type = ADL_DOT_ANALOG;
        d->connector = ADL_DISPLAY_CONTYPE_VGA;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
        d->display_type = ADL_DT_MONITOR;
        d->output_type = ADL_DOT_SVIDEO;
        d->connector = ADL_DISPLAY_CONTYPE_SVIDEO;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
        d->display_type = ADL_DT_MONITOR;
        d->output_type = ADL_DOT_COMPOSITE;
        d->connector = ADL_DISPLAY_CONTYPE_COMPOSITE;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
        d->display_type = ADL_DT_MONITOR;
        d->output_type = ADL_DOT_ANALOG;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
        d->connector = ADL_DISPLAY_CONTYPE_DVI_D;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
        d->connector = ADL_DISPLAY_CONTYPE_HDMI_TYPE_A;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
        d->connector = ADL_DISPLAY_CONTYPE_DISPLAYPORT;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        d->display_type = ADL_DT_LCD_PANEL;
        d->connector = ADL_DISPLAY_CONTYPE_EDP;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL:
        d->connector = ADL_DISPLAY_CONTYPE_USB_TYPE_C;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:
        d->connector = ADL_DISPLAY_CONTYPE_WIRELESSDISPLAY;
        break;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        d->display_type = ADL_DT_LCD_PANEL;
        break;
    default:
        d->display_type = ADL_DT_MONITOR;
        d->output_type = ADL_DOT_UNKNOWN;
        break;
    }
}

// The three letters of an EDID manufacturer id. DisplayConfig gives bytes 8 and 9 of the EDID as one
// little-endian USHORT; the EDID stores the id big-endian, five bits per letter, 1 = 'A'.
void ManufacturerText(USHORT edid, char* out)
{
    unsigned id = (unsigned)((edid >> 8) | ((edid & 0xff) << 8));
    char a = (char)('@' + ((id >> 10) & 31)), b = (char)('@' + ((id >> 5) & 31)), c = (char)('@' + (id & 31));
    out[0] = '\0';
    if (a > '@' && a <= 'Z' && b > '@' && b <= 'Z' && c > '@' && c <= 'Z') {
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = '\0';
    }
}

void AddDisplays(AdlSnapshot* s)
{
    if (s->gpu_count == 0) {
        return;
    }
    UINT32 paths = 0, modes = 0;
    DISPLAYCONFIG_PATH_INFO* path = nullptr;
    DISPLAYCONFIG_MODE_INFO* mode = nullptr;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; attempt++) {
        HeapFree(GetProcessHeap(), 0, path);
        HeapFree(GetProcessHeap(), 0, mode);
        path = nullptr;
        mode = nullptr;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || paths == 0) {
            return;
        }
        path = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, paths * sizeof(*path));
        mode = (DISPLAYCONFIG_MODE_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (modes + 1) * sizeof(*mode));
        if (!path || !mode) {
            status = ERROR_OUTOFMEMORY;
            break;
        }
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path, &modes, mode, nullptr);
    }
    if (status == ERROR_SUCCESS) {
        int next_index[ADL_MODEL_MAX_GPUS] = {};
        for (UINT32 p = 0; p < paths && s->display_count < ADL_MODEL_MAX_DISPLAYS; p++) {
            const DISPLAYCONFIG_PATH_INFO* pi = &path[p];
            int g = FindGpu(s, pi->sourceInfo.adapterId);
            if (g < 0) {
                continue;
            }
            AdlDisplay* d = &s->displays[s->display_count];
            memset(d, 0, sizeof(*d));
            d->gpu = g;
            d->target_id = (int32_t)pi->targetInfo.id;

            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = pi->sourceInfo.adapterId;
            source.header.id = pi->sourceInfo.id;
            char gdi[32] = {};
            if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
                ToText(gdi, sizeof(gdi), source.viewGdiDeviceName);
            }
            // The entry of the path's source; the GPU's first entry when no source name matches.
            d->entry = -1;
            for (int e = 0; e < s->entry_count && d->entry < 0; e++) {
                if (s->entries[e].gpu == g && gdi[0] && strcmp(s->entries[e].gdi_name, gdi) == 0) {
                    d->entry = e;
                }
            }
            for (int e = 0; e < s->entry_count && d->entry < 0; e++) {
                if (s->entries[e].gpu == g) {
                    d->entry = e;
                }
            }
            if (d->entry < 0) {
                continue;
            }

            DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = pi->targetInfo.adapterId;
            target.header.id = pi->targetInfo.id;
            DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech = pi->targetInfo.outputTechnology;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
                ToText(d->name, sizeof(d->name), target.monitorFriendlyDeviceName);
                if (target.flags.edidIdsValid) {
                    ManufacturerText(target.edidManufactureId, d->manufacturer);
                }
                tech = target.outputTechnology;
            }
            DescribeOutput(tech, d);

            UINT32 m = pi->sourceInfo.modeInfoIdx;
            if (m != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && m < modes &&
                mode[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                d->x = mode[m].sourceMode.position.x;
                d->y = mode[m].sourceMode.position.y;
                d->width = (int32_t)mode[m].sourceMode.width;
                d->height = (int32_t)mode[m].sourceMode.height;
            }
            if (pi->targetInfo.refreshRate.Denominator != 0) {
                d->refresh_hz = (float)((double)pi->targetInfo.refreshRate.Numerator /
                                        (double)pi->targetInfo.refreshRate.Denominator);
            }
            switch (pi->targetInfo.rotation) {
            case DISPLAYCONFIG_ROTATION_ROTATE90: d->orientation = 90; break;
            case DISPLAYCONFIG_ROTATION_ROTATE180: d->orientation = 180; break;
            case DISPLAYCONFIG_ROTATION_ROTATE270: d->orientation = 270; break;
            default: d->orientation = 0; break;
            }
            d->index = next_index[g]++;
            s->display_count++;
        }
    }
    HeapFree(GetProcessHeap(), 0, path);
    HeapFree(GetProcessHeap(), 0, mode);
}

}  // namespace

void AdlDiscover(AdlSnapshot* s)
{
    memset(s, 0, sizeof(*s));
    Candidate* candidates =
        (Candidate*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ADL_MODEL_MAX_GPUS * sizeof(Candidate));
    if (!candidates) {
        return;
    }
    int count = FindCandidates(candidates, ADL_MODEL_MAX_GPUS);
    if (count > 0) {
        MatchRunningAdapters(candidates, count);
    }
    for (int c = 0; c < count; c++) {
        if (candidates[c].matched) {
            s->gpus[s->gpu_count++] = candidates[c].gpu;
        }
    }
    HeapFree(GetProcessHeap(), 0, candidates);
    AddEntries(s);
    AddDisplays(s);
}
