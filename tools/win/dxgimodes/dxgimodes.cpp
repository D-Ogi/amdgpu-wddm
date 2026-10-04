// dxgimodes - what display modes DXGI and dxgkrnl report for each output, per format. No window, no device.
//
// 3DMark Steel Nomad stopped with "Display mode list not found for given format" on the BC-250 adapter
// (lab session native-caps349). This tool shows the layers that answer such a query:
//   CCD    QueryDisplayConfig: the desktop's source pixel format (DISPLAYCONFIG_PIXELFORMAT, 4 = 32BPP,
//          5 = NONGDI). It must stay 32BPP whatever the mode list holds.
//   KMTA   D3DKMTEnumAdapters2 + D3DKMTGetDisplayModeList for every adapter and VidPN source. This needs no
//          desktop session, so it also answers from an SSH shell in session 0.
//   KMT    D3DKMTOpenAdapterFromGdiDisplayName + D3DKMTGetDisplayModeList for each DXGI output.
//   DXGI   IDXGIOutput1::GetDisplayModeList1 per DXGI format, flags 0 and DXGI_ENUM_MODES_SCALING. This is
//          what an application gets.
//
//   dxgimodes.exe [--all] [--out FILE] [--skip-ccd] [--skip-kmt] [--skip-dxgi]
//     --all        print every mode instead of the first ones of each list
//     --out FILE   write every line to FILE too (for a run in the console session through a scheduled task)
//     --skip-*     leave one layer out
//
// Every line is flushed at once, so a crash loses no output. "STAGE ..." names the call about to run, and an
// unhandled exception prints "CRASH ..." with the code, the address, its module and the last stage.
// Exit code 0 when every query succeeded, 1 when a call failed, 2 for a bad argument, 3 after a crash.
// Read-only: nothing here changes a mode, an owner or a setting.
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>       // NTSTATUS; d3dkmthk.h needs it and does not include it
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct FormatName { DXGI_FORMAT dxgi; const char* name; };

// The formats games and benchmarks ask GetDisplayModeList for. B8G8R8X8 is not a DXGI display format; it is
// here as a negative control (expected 0 on any driver).
static const FormatName kFormats[] = {
    {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB"},
    {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB"},
    {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM"},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT"},
    {DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8_UNORM"},
};

static const UINT kModeCap = 4096;  // a list longer than this is reported, not allocated

static int g_all = 0;
static int g_failures = 0;
static FILE* g_out = nullptr;
static char g_stage[160] = "start";

static void Line(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (g_out) {
        fputs(text, g_out);
        fputc('\n', g_out);
        fflush(g_out);
    }
}

static void Stage(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_stage, sizeof(g_stage), format, args);
    va_end(args);
    Line("STAGE %s", g_stage);
}

// Only fixed buffers and the C runtime here: the heap or a COM object may be what failed.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
    void* address = r ? r->ExceptionAddress : nullptr;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module))
        GetModuleFileNameA(module, name, sizeof(name));
    Line("CRASH code=0x%08lX address=%p module=%s offset=0x%llX access=%llu target=0x%llX stage=%s",
         r ? (unsigned long)r->ExceptionCode : 0ul, address, name,
         module ? (unsigned long long)((const char*)address - (const char*)module) : 0ull,
         r && r->NumberParameters >= 1 ? (unsigned long long)r->ExceptionInformation[0] : 0ull,
         r && r->NumberParameters >= 2 ? (unsigned long long)r->ExceptionInformation[1] : 0ull, g_stage);
    if (g_out) fclose(g_out);
    ExitProcess(3);
}

static const char* DdiFormatName(UINT f)  // values from d3dukmdt.h, never typed by hand
{
    switch ((D3DDDIFORMAT)f) {
    case D3DDDIFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DDDIFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DDDIFMT_A2B10G10R10: return "A2B10G10R10";
    case D3DDDIFMT_A8B8G8R8: return "A8B8G8R8";
    case D3DDDIFMT_X8B8G8R8: return "X8B8G8R8";
    case D3DDDIFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DDDIFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DDDIFMT_A2B10G10R10_XR_BIAS: return "A2B10G10R10_XR_BIAS";
    default: return "other";
    }
}

// The mode list of one adapter handle and source, counted per D3DDDIFORMAT. Tag names the caller's line.
static void PrintModeList(const char* tag, const char* who, D3DKMT_HANDLE adapter, UINT source)
{
    D3DKMT_GETDISPLAYMODELIST list = {};
    list.hAdapter = adapter;
    list.VidPnSourceId = source;
    Stage("%s %s source %u D3DKMTGetDisplayModeList count", tag, who, source);
    NTSTATUS st = D3DKMTGetDisplayModeList(&list);  // ModeCount 0, pModeList NULL: the count query
    std::vector<D3DKMT_DISPLAYMODE> modes;
    UINT count = list.ModeCount;
    if (st == STATUS_SUCCESS && count > kModeCap) {
        Line("%s %s source=%u count=%u above the cap %u, list not read", tag, who, source, count, kModeCap);
        ++g_failures;
        return;
    }
    if (st == STATUS_SUCCESS && count) {
        modes.resize(count);
        list.pModeList = modes.data();
        list.ModeCount = count;
        Stage("%s %s source %u D3DKMTGetDisplayModeList list %u", tag, who, source, count);
        st = D3DKMTGetDisplayModeList(&list);
        if (list.ModeCount < count) count = list.ModeCount;  // the list may shrink between the two calls
    }
    if (st != STATUS_SUCCESS) count = 0;
    Line("%s %s source=%u status=0x%08lX modes=%u", tag, who, source, (unsigned long)st, count);
    if (st != STATUS_SUCCESS) {
        ++g_failures;
        return;
    }
    UINT seen[16] = {}, counts[16] = {}, kinds = 0;
    for (UINT i = 0; i < count; ++i) {
        UINT k = 0;
        while (k < kinds && seen[k] != (UINT)modes[i].Format) ++k;
        if (k == kinds && kinds < 16) seen[kinds++] = (UINT)modes[i].Format;
        if (k < 16) ++counts[k];
    }
    for (UINT k = 0; k < kinds; ++k)
        Line("%s %s source=%u format=%u(%s) modes=%u", tag, who, source, seen[k], DdiFormatName(seen[k]), counts[k]);
    for (UINT i = 0; i < count && (g_all || i < 8); ++i)
        Line("%s %s source=%u mode[%u] %ux%u format=%u(%s) refresh=%u/%u orientation=%u fixed=%u flags=0x%08X", tag,
             who, source, i, modes[i].Width, modes[i].Height, (UINT)modes[i].Format,
             DdiFormatName((UINT)modes[i].Format), modes[i].RefreshRate.Numerator, modes[i].RefreshRate.Denominator,
             (UINT)modes[i].DisplayOrientation, modes[i].DisplayFixedOutput, *(const UINT*)&modes[i].Flags);
}

// Every adapter and source the kernel knows, with no display session needed.
static void PrintKmtAdapters()
{
    D3DKMT_ENUMADAPTERS2 e = {};
    Stage("KMTA D3DKMTEnumAdapters2 count");
    NTSTATUS st = D3DKMTEnumAdapters2(&e);
    if (st != STATUS_SUCCESS || e.NumAdapters == 0 || e.NumAdapters > 64) {
        Line("KMTA enum status=0x%08lX adapters=%lu", (unsigned long)st, (unsigned long)e.NumAdapters);
        if (st != STATUS_SUCCESS) ++g_failures;
        return;
    }
    std::vector<D3DKMT_ADAPTERINFO> info(e.NumAdapters);
    e.pAdapters = info.data();
    Stage("KMTA D3DKMTEnumAdapters2 list %lu", (unsigned long)e.NumAdapters);
    st = D3DKMTEnumAdapters2(&e);
    Line("KMTA enum status=0x%08lX adapters=%lu", (unsigned long)st, (unsigned long)e.NumAdapters);
    if (st != STATUS_SUCCESS) {
        ++g_failures;
        return;
    }
    for (ULONG a = 0; a < e.NumAdapters && a < info.size(); ++a) {
        char who[64];
        snprintf(who, sizeof(who), "luid=%08lX:%08lX", (unsigned long)info[a].AdapterLuid.HighPart,
                 (unsigned long)info[a].AdapterLuid.LowPart);
        Line("KMTA %s adapter=%lu sources=%lu", who, (unsigned long)a, (unsigned long)info[a].NumOfSources);
        for (UINT s = 0; s < info[a].NumOfSources && s < 16; ++s)
            PrintModeList("KMTA", who, info[a].hAdapter, s);
    }
    for (ULONG a = 0; a < e.NumAdapters && a < info.size(); ++a) {
        D3DKMT_CLOSEADAPTER close = {info[a].hAdapter};
        D3DKMTCloseAdapter(&close);
    }
}

static void PrintKmtOutput(const wchar_t* gdiName)
{
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {};
    wcsncpy_s(open.DeviceName, gdiName, _TRUNCATE);
    char who[64];
    snprintf(who, sizeof(who), "%ls", gdiName);
    Stage("KMT %s D3DKMTOpenAdapterFromGdiDisplayName", who);
    NTSTATUS st = D3DKMTOpenAdapterFromGdiDisplayName(&open);
    if (st != STATUS_SUCCESS) {
        Line("KMT %s open status=0x%08lX", who, (unsigned long)st);
        ++g_failures;
        return;
    }
    Line("KMT %s luid=%08lX:%08lX source=%u", who, (unsigned long)open.AdapterLuid.HighPart,
         (unsigned long)open.AdapterLuid.LowPart, open.VidPnSourceId);
    PrintModeList("KMT", who, open.hAdapter, open.VidPnSourceId);
    D3DKMT_CLOSEADAPTER close = {open.hAdapter};
    D3DKMTCloseAdapter(&close);
}

static UINT PrintDxgiList(IDXGIOutput1* out, const char* who, const FormatName& f, UINT flags)
{
    UINT count = 0;
    Stage("DXGI %s %s flags 0x%X GetDisplayModeList1 count", who, f.name, flags);
    HRESULT hr = out->GetDisplayModeList1(f.dxgi, flags, &count, nullptr);
    std::vector<DXGI_MODE_DESC1> modes;
    if (SUCCEEDED(hr) && count > kModeCap) {
        Line("DXGI %s format=%u(%s) flags=0x%X count=%u above the cap, list not read", who, (UINT)f.dxgi, f.name,
             flags, count);
        return count;
    }
    if (SUCCEEDED(hr) && count) {
        modes.resize(count);
        Stage("DXGI %s %s flags 0x%X GetDisplayModeList1 list %u", who, f.name, flags, count);
        hr = out->GetDisplayModeList1(f.dxgi, flags, &count, modes.data());
        if (count > modes.size()) count = (UINT)modes.size();
    }
    if (FAILED(hr)) count = 0;
    Line("DXGI %s format=%u(%s) flags=0x%X hr=0x%08lX modes=%u", who, (UINT)f.dxgi, f.name, flags, (unsigned long)hr,
         count);
    if (FAILED(hr) && hr != DXGI_ERROR_MORE_DATA) ++g_failures;
    for (UINT i = 0; i < count && (g_all || i < 3); ++i)
        Line("DXGI %s format=%u mode[%u] %ux%u refresh=%u/%u scanline=%u scaling=%u stereo=%d", who, (UINT)f.dxgi, i,
             modes[i].Width, modes[i].Height, modes[i].RefreshRate.Numerator, modes[i].RefreshRate.Denominator,
             (UINT)modes[i].ScanlineOrdering, (UINT)modes[i].Scaling, (int)modes[i].Stereo);
    return count;
}

static void PrintCcd()
{
    UINT32 paths = 0, modes = 0;
    Stage("CCD GetDisplayConfigBufferSizes");
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes);
    if (rc != ERROR_SUCCESS || paths == 0 || modes == 0 || paths > 256 || modes > 512) {
        // Session 0 has no desktop: an error or zero paths is an answer, not a failure of the tool.
        Line("CCD sizes status=%ld paths=%u modes=%u (no query)", rc, paths, modes);
        return;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    Stage("CCD QueryDisplayConfig %u %u", paths, modes);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr);
    Line("CCD status=%ld paths=%u modes=%u", rc, paths, modes);
    if (rc != ERROR_SUCCESS) return;
    for (UINT32 i = 0; i < modes && i < m.size(); ++i)
        if (m[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            Line("CCD source id=%u luid=%08lX:%08lX %ux%u pixelformat=%u", m[i].id,
                 (unsigned long)m[i].adapterId.HighPart, (unsigned long)m[i].adapterId.LowPart,
                 m[i].sourceMode.width, m[i].sourceMode.height, (UINT)m[i].sourceMode.pixelFormat);
}

static void PrintDxgi(int kmt, UINT* outputsTotal, UINT* emptyLists)
{
    IDXGIFactory1* factory = nullptr;
    Stage("DXGI CreateDXGIFactory1");
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory) {
        Line("DXGI CreateDXGIFactory1 hr=0x%08lX", (unsigned long)hr);
        ++g_failures;
        return;
    }
    for (UINT a = 0; a < 16; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        Stage("DXGI EnumAdapters1 %u", a);
        hr = factory->EnumAdapters1(a, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !adapter) {
            Line("DXGI EnumAdapters1 %u hr=0x%08lX", a, (unsigned long)hr);
            ++g_failures;
            break;
        }
        DXGI_ADAPTER_DESC1 ad = {};
        Stage("DXGI adapter %u GetDesc1", a);
        adapter->GetDesc1(&ad);
        Line("ADAPTER %u \"%ls\" vendor=0x%04X device=0x%04X luid=%08lX:%08lX flags=0x%X", a, ad.Description,
             ad.VendorId, ad.DeviceId, (unsigned long)ad.AdapterLuid.HighPart, (unsigned long)ad.AdapterLuid.LowPart,
             ad.Flags);
        for (UINT o = 0; o < 16; ++o) {
            IDXGIOutput* output = nullptr;
            Stage("DXGI adapter %u EnumOutputs %u", a, o);
            hr = adapter->EnumOutputs(o, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                if (o == 0) Line("OUTPUT %u none (session without a desktop, or no display on this adapter)", a);
                break;
            }
            if (FAILED(hr) || !output) {
                Line("OUTPUT %u.%u EnumOutputs hr=0x%08lX", a, o, (unsigned long)hr);
                ++g_failures;
                break;
            }
            ++*outputsTotal;
            DXGI_OUTPUT_DESC od = {};
            Stage("DXGI output %u.%u GetDesc", a, o);
            output->GetDesc(&od);
            char who[64];
            snprintf(who, sizeof(who), "%ls", od.DeviceName);
            const RECT& r = od.DesktopCoordinates;
            Line("OUTPUT %u.%u %s attached=%d desktop=%ldx%ld rotation=%u", a, o, who, (int)od.AttachedToDesktop,
                 r.right - r.left, r.bottom - r.top, (UINT)od.Rotation);
            IDXGIOutput6* out6 = nullptr;
            Stage("DXGI output %u.%u IDXGIOutput6", a, o);
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&out6)) && out6) {
                DXGI_OUTPUT_DESC1 d1 = {};
                Stage("DXGI output %u.%u GetDesc1", a, o);
                hr = out6->GetDesc1(&d1);
                Line("OUTPUT %u.%u desc1 hr=0x%08lX colorspace=%u bits=%u luminance min=%.4f max=%.1f full=%.1f", a, o,
                     (unsigned long)hr, (UINT)d1.ColorSpace, d1.BitsPerColor, d1.MinLuminance, d1.MaxLuminance,
                     d1.MaxFullFrameLuminance);
                UINT support = 0;
                Stage("DXGI output %u.%u CheckHardwareCompositionSupport", a, o);
                hr = out6->CheckHardwareCompositionSupport(&support);
                Line("OUTPUT %u.%u hardware-composition hr=0x%08lX support=0x%X", a, o, (unsigned long)hr, support);
                out6->Release();
            }
            if (kmt) PrintKmtOutput(od.DeviceName);
            IDXGIOutput1* out1 = nullptr;
            Stage("DXGI output %u.%u IDXGIOutput1", a, o);
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1)) && out1) {
                for (const FormatName& f : kFormats) {
                    UINT plain = PrintDxgiList(out1, who, f, 0);
                    PrintDxgiList(out1, who, f, DXGI_ENUM_MODES_SCALING);
                    if (f.dxgi != DXGI_FORMAT_B8G8R8X8_UNORM && plain == 0) ++*emptyLists;
                }
                out1->Release();
            } else {
                Line("DXGI %s no IDXGIOutput1", who);
                ++g_failures;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
}

int main(int argc, char** argv)
{
    int ccd = 1, kmt = 1, dxgi = 1;
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(CrashFilter);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--all")) g_all = 1;
        else if (!strcmp(argv[i], "--skip-ccd")) ccd = 0;
        else if (!strcmp(argv[i], "--skip-kmt")) kmt = 0;
        else if (!strcmp(argv[i], "--skip-dxgi")) dxgi = 0;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            if (fopen_s(&g_out, argv[++i], "w") != 0) g_out = nullptr;
            if (!g_out) { fprintf(stderr, "cannot open %s\n", argv[i]); return 2; }
        } else {
            fprintf(stderr, "usage: dxgimodes.exe [--all] [--out FILE] [--skip-ccd] [--skip-kmt] [--skip-dxgi]\n");
            return 2;
        }
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Line("START dxgimodes session=%lu all=%d ccd=%d kmt=%d dxgi=%d", (unsigned long)session, g_all, ccd, kmt, dxgi);

    if (ccd) PrintCcd();
    if (kmt) PrintKmtAdapters();
    UINT outputsTotal = 0, emptyLists = 0;
    if (dxgi) PrintDxgi(kmt, &outputsTotal, &emptyLists);
    Line("SUMMARY outputs=%u empty-display-format-lists=%u failures=%d", outputsTotal, emptyLists, g_failures);
    if (g_out) fclose(g_out);
    return g_failures ? 1 : 0;
}
