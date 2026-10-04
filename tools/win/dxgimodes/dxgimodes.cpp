// dxgimodes - what display modes DXGI and dxgkrnl report for each output, per format. No window, no device.
//
// 3DMark Steel Nomad stopped with "Display mode list not found for given format" on the BC-250 adapter
// (lab session native-caps349). This tool shows the two layers that answer such a query:
//   KMT   D3DKMTGetDisplayModeList: the source modes dxgkrnl collected from the miniport's VidPN source mode
//         sets, with the D3DDDIFORMAT each one carries. This is what the kernel driver said.
//   DXGI  IDXGIOutput1::GetDisplayModeList1 per DXGI format, flags 0 and DXGI_ENUM_MODES_SCALING. This is
//         what an application gets.
//   CCD   QueryDisplayConfig: the desktop's source pixel format (DISPLAYCONFIG_PIXELFORMAT, 4 = 32BPP,
//         5 = NONGDI). It must stay 32BPP whatever the mode list holds.
//
//   dxgimodes.exe [--all]      --all prints every mode instead of the first three of each list
//
// Output is line based: "KMT ...", "DXGI ...", "CCD ...", "SUMMARY ...". Exit code 0 when every output was
// queried, 1 when a call failed. Read-only: nothing here changes a mode, an owner or a setting.
#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>       // NTSTATUS; d3dkmthk.h needs it and does not include it
#include <d3dkmthk.h>
#include <dxgi1_6.h>
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

static int g_all = 0;
static int g_failures = 0;

static void PrintKmt(const wchar_t* gdiName)
{
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {};
    wcsncpy_s(open.DeviceName, gdiName, _TRUNCATE);
    NTSTATUS st = D3DKMTOpenAdapterFromGdiDisplayName(&open);
    if (st != STATUS_SUCCESS) {
        printf("KMT %ls open status=0x%08lX\n", gdiName, (unsigned long)st);
        ++g_failures;
        return;
    }
    D3DKMT_GETDISPLAYMODELIST list = {};
    list.hAdapter = open.hAdapter;
    list.VidPnSourceId = open.VidPnSourceId;
    st = D3DKMTGetDisplayModeList(&list);  // ModeCount 0, pModeList NULL: the count query
    std::vector<D3DKMT_DISPLAYMODE> modes;
    if (st == STATUS_SUCCESS && list.ModeCount) {
        modes.resize(list.ModeCount);
        list.pModeList = modes.data();
        st = D3DKMTGetDisplayModeList(&list);
    }
    printf("KMT %ls luid=%08lX:%08lX source=%u status=0x%08lX modes=%u\n", gdiName,
           (unsigned long)open.AdapterLuid.HighPart, (unsigned long)open.AdapterLuid.LowPart,
           open.VidPnSourceId, (unsigned long)st, st == STATUS_SUCCESS ? list.ModeCount : 0u);
    if (st != STATUS_SUCCESS) ++g_failures;
    else {
        // Count per D3DDDIFORMAT, then list the modes.
        UINT seen[8] = {}, counts[8] = {}, kinds = 0;
        for (UINT i = 0; i < list.ModeCount; ++i) {
            UINT k = 0;
            while (k < kinds && seen[k] != (UINT)modes[i].Format) ++k;
            if (k == kinds && kinds < 8) seen[kinds++] = (UINT)modes[i].Format;
            if (k < 8) ++counts[k];
        }
        for (UINT k = 0; k < kinds; ++k)
            printf("KMT %ls format=%u(%s) modes=%u\n", gdiName, seen[k], DdiFormatName(seen[k]), counts[k]);
        for (UINT i = 0; i < list.ModeCount && (g_all || i < 8); ++i)
            printf("KMT %ls mode[%u] %ux%u format=%u(%s) refresh=%u/%u orientation=%u fixed=%u flags=0x%08X\n",
                   gdiName, i, modes[i].Width, modes[i].Height, (UINT)modes[i].Format,
                   DdiFormatName((UINT)modes[i].Format), modes[i].RefreshRate.Numerator,
                   modes[i].RefreshRate.Denominator, (UINT)modes[i].DisplayOrientation,
                   modes[i].DisplayFixedOutput, *(const UINT*)&modes[i].Flags);
    }
    D3DKMT_CLOSEADAPTER close = {open.hAdapter};
    D3DKMTCloseAdapter(&close);
}

static void PrintDxgiList(IDXGIOutput1* out, const wchar_t* gdiName, const FormatName& f, UINT flags)
{
    UINT count = 0;
    HRESULT hr = out->GetDisplayModeList1(f.dxgi, flags, &count, nullptr);
    std::vector<DXGI_MODE_DESC1> modes;
    if (SUCCEEDED(hr) && count) {
        modes.resize(count);
        hr = out->GetDisplayModeList1(f.dxgi, flags, &count, modes.data());
    }
    printf("DXGI %ls format=%u(%s) flags=0x%X hr=0x%08lX modes=%u\n", gdiName, (UINT)f.dxgi, f.name, flags,
           (unsigned long)hr, SUCCEEDED(hr) ? count : 0u);
    if (FAILED(hr) && hr != DXGI_ERROR_MORE_DATA) ++g_failures;
    for (UINT i = 0; SUCCEEDED(hr) && i < count && (g_all || i < 3); ++i)
        printf("DXGI %ls format=%u mode[%u] %ux%u refresh=%u/%u scanline=%u scaling=%u stereo=%d\n", gdiName,
               (UINT)f.dxgi, i, modes[i].Width, modes[i].Height, modes[i].RefreshRate.Numerator,
               modes[i].RefreshRate.Denominator, (UINT)modes[i].ScanlineOrdering, (UINT)modes[i].Scaling,
               (int)modes[i].Stereo);
}

static void PrintCcd()
{
    UINT32 paths = 0, modes = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes);
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (rc == ERROR_SUCCESS) rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr);
    printf("CCD status=%ld paths=%u modes=%u\n", rc, paths, modes);
    if (rc != ERROR_SUCCESS) { ++g_failures; return; }
    for (UINT32 i = 0; i < modes; ++i)
        if (m[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            printf("CCD source id=%u luid=%08lX:%08lX %ux%u pixelformat=%u\n", m[i].id,
                   (unsigned long)m[i].adapterId.HighPart, (unsigned long)m[i].adapterId.LowPart,
                   m[i].sourceMode.width, m[i].sourceMode.height, (UINT)m[i].sourceMode.pixelFormat);
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--all")) g_all = 1;
        else { fprintf(stderr, "usage: dxgimodes.exe [--all]\n"); return 2; }
    }
    PrintCcd();

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr)) { printf("DXGI CreateDXGIFactory1 hr=0x%08lX\n", (unsigned long)hr); return 1; }

    UINT outputsTotal = 0, emptyLists = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad = {};
        adapter->GetDesc1(&ad);
        printf("ADAPTER %u \"%ls\" vendor=0x%04X device=0x%04X luid=%08lX:%08lX flags=0x%X\n", a, ad.Description,
               ad.VendorId, ad.DeviceId, (unsigned long)ad.AdapterLuid.HighPart,
               (unsigned long)ad.AdapterLuid.LowPart, ad.Flags);
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            ++outputsTotal;
            DXGI_OUTPUT_DESC od = {};
            output->GetDesc(&od);
            const RECT& r = od.DesktopCoordinates;
            printf("OUTPUT %u.%u %ls attached=%d desktop=%ldx%ld rotation=%u\n", a, o, od.DeviceName,
                   (int)od.AttachedToDesktop, r.right - r.left, r.bottom - r.top, (UINT)od.Rotation);
            IDXGIOutput6* out6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&out6))) {
                DXGI_OUTPUT_DESC1 d1 = {};
                hr = out6->GetDesc1(&d1);
                printf("OUTPUT %u.%u desc1 hr=0x%08lX colorspace=%u bits=%u luminance min=%.4f max=%.1f full=%.1f\n",
                       a, o, (unsigned long)hr, (UINT)d1.ColorSpace, d1.BitsPerColor, d1.MinLuminance,
                       d1.MaxLuminance, d1.MaxFullFrameLuminance);
                UINT support = 0;
                hr = out6->CheckHardwareCompositionSupport(&support);
                printf("OUTPUT %u.%u hardware-composition hr=0x%08lX support=0x%X\n", a, o, (unsigned long)hr, support);
                out6->Release();
            }
            PrintKmt(od.DeviceName);
            IDXGIOutput1* out1 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1))) {
                for (const FormatName& f : kFormats) {
                    PrintDxgiList(out1, od.DeviceName, f, 0);
                    PrintDxgiList(out1, od.DeviceName, f, DXGI_ENUM_MODES_SCALING);
                    UINT count = 0;
                    if (f.dxgi != DXGI_FORMAT_B8G8R8X8_UNORM &&
                        SUCCEEDED(out1->GetDisplayModeList1(f.dxgi, 0, &count, nullptr)) && count == 0)
                        ++emptyLists;
                }
                out1->Release();
            } else {
                printf("DXGI %ls no IDXGIOutput1\n", od.DeviceName);
                ++g_failures;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    printf("SUMMARY outputs=%u empty-display-format-lists=%u failures=%d\n", outputsTotal, emptyLists, g_failures);
    return g_failures ? 1 : 0;
}
