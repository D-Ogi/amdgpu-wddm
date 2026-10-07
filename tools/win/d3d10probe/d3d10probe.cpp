// d3d10probe: which user-mode driver serves a Direct3D 10.0 application on this adapter, and whether it renders
// correctly (BD-081).
//
// The probe calls D3D10CreateDeviceAndSwapChain, the entry point of the Direct3D 10.0 runtime (d3d10.dll and
// d3d10core.dll). It prints the adapter and the loaded modules that name a graphics stack, so a lab run shows which
// UMD served it: amdgpu_wddm_d3d11.dll (the application GPU UMD), bc250d3d.dll (the CPU UMD), bc250d3d_zink.dll (the
// hosted GPU UMD) or another.
//
// --trace-entry also shows how the runtime opened that UMD. Before the device is created, the probe replaces the
// GetProcAddress import of the runtime DLLs in its own process (d3d10, d3d10core, d3d10_1, d3d10_1core, d3d11, dxgi)
// with a function that logs every "OpenAdapter*" name the runtime asks for, wraps OpenAdapter10 and OpenAdapter10_2,
// and then wraps CalcPrivateDeviceSize and CreateDevice of the adapter table the UMD returns. Each call is logged
// with its Interface and Version and, for CreateDevice, the 3D pipeline level from Flags, and then passed through
// unchanged. Nothing outside this process is touched. On Windows 11 (build 26100 and later) the measured answer is
// OpenAdapter10_2 and a D3D11-family interface, not OpenAdapter10 (BD-081).
//
// The test: a 256x256 R8G8B8A8_UNORM render target, a clear and one triangle over the top-left half from a vertex
// buffer through an input layout and a vertex and a pixel shader (compiled at run time with the system's
// d3dcompiler_47.dll), copied to a staging texture and read back. The check is exact: every pixel is either the
// clear colour or the triangle colour, sample pixels away from the edge must match, and the triangle covers exactly
// 32 640 pixels: the centres (x + 0.5, y + 0.5) with x + y < 255. The centres with x + y = 255 lie on the diagonal,
// which is a right edge, so the top-left rule of D3D10 leaves them out (WARP and a hardware driver both give 32 640).
// Then --frames frames are drawn into the swap chain of a window and presented (DXGI Present, the path every D3D10
// application takes to the screen). The window stays hidden unless --show is given.
//
// Exit codes: 0 = rendered correctly (and every Present succeeded), 3 = wrong pixels, 4 = a Present failed,
// 2 = no device, 1 = usage or a probe error (no shader compiler, a resource that could not be created).
#include <windows.h>
#pragma warning(push)
#pragma warning(disable : 4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>  // before d3d10.h, which it includes; the probe still calls the D3D10.0 entry points only
#include <d3d10.h>
#include <d3d11.h>
#include <d3d10umddi.h>  // the WDK header: the adapter and device-creation arguments that --trace-entry logs
#pragma warning(pop)
#include <d3dcompiler.h>
#include <dxgi.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const UINT kSize = 256;
// Colours as UNORM bytes; the shader and the clear use byte / 255, which converts back exactly.
const BYTE kClear[4] = {0x20, 0x40, 0x80, 0xFF};
const BYTE kTri[4] = {0xF0, 0xA0, 0x10, 0xFF};

FILE *g_out = stdout;

void Say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_out != stdout) {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
        fflush(g_out);
    }
}

void Modules() {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    const char *keys[] = {"d3d10", "d3d11", "d3d12", "dxgi", "bc250", "amdgpu", "dxvk", "vkd3d", "vulkan", "radv",
                          "zink", "lvp", "warp", "nvwgf", "aticfx", "amdxx", "igd"};
    const DWORD n = need / sizeof(HMODULE) < 1024 ? need / sizeof(HMODULE) : 1024;
    for (DWORD i = 0; i < n; i++) {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        char lower[MAX_PATH] = {};
        for (int k = 0; path[k] && k < MAX_PATH - 1; k++) lower[k] = (char)tolower((unsigned char)path[k]);
        const char *base = strrchr(lower, '\\');
        base = base ? base + 1 : lower;
        for (const char *key : keys) {
            if (strstr(base, key)) {
                Say("module %s\n", path);
                break;
            }
        }
    }
}

// The UMD that served the device, by its module name. "none" when none of ours is loaded.
const char *ServedBy() {
    if (GetModuleHandleA("bc250d3d_zink.dll")) return "hosted GPU UMD (bc250d3d_zink.dll)";
    if (GetModuleHandleA("amdgpu_wddm_d3d11.dll")) return "application GPU UMD (amdgpu_wddm_d3d11.dll)";
    if (GetModuleHandleA("bc250d3d.dll")) return "CPU UMD (bc250d3d.dll)";
    if (GetModuleHandleA("d3d10warp.dll")) return "WARP (d3d10warp.dll)";
    return "none of the named UMDs";
}

// ---------------------------------------------------------------- --trace-entry

typedef HRESULT(APIENTRY *OpenAdapterFn)(D3D10DDIARG_OPENADAPTER *);
typedef FARPROC(WINAPI *GetProcAddressFn)(HMODULE, LPCSTR);
GetProcAddressFn g_realGetProcAddress;
OpenAdapterFn g_realOpen10, g_realOpen102;
PFND3D10DDI_CREATEDEVICE g_realCreateDevice;
PFND3D10DDI_CALCPRIVATEDEVICESIZE g_realCalcSize;

const char *PipelineLevel(UINT flags) {
    switch (D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(flags)) {
    case D3D11DDI_3DPIPELINELEVEL_10_0: return "10_0";
    case D3D11DDI_3DPIPELINELEVEL_10_1: return "10_1";
    case D3D11DDI_3DPIPELINELEVEL_11_0: return "11_0";
    case D3D11_1DDI_3DPIPELINELEVEL_11_1: return "11_1";
    case D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0: return "12_0";
    case D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1: return "12_1";
    }
    return "other";
}

SIZE_T APIENTRY TraceCalcSize(D3D10DDI_HADAPTER adapter, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args) {
    Say("entry: CalcPrivateDeviceSize interface %08X version %08X\n", args ? args->Interface : 0,
        args ? args->Version : 0);
    return g_realCalcSize(adapter, args);
}

HRESULT APIENTRY TraceCreateDevice(D3D10DDI_HADAPTER adapter, D3D10DDIARG_CREATEDEVICE *args) {
    const HRESULT hr = g_realCreateDevice(adapter, args);
    // The interface is a major.minor pair: 10.1 is D3D10.0, 11.15 is D3D11.1, 11.17 and later are WDDM 2.x and 3.x.
    Say("entry: CreateDevice interface %08X (%u.%u) version %08X pipeline level %s -> 0x%08lX\n",
        args ? args->Interface : 0, args ? args->Interface >> 16 : 0, args ? args->Interface & 0xFFFF : 0,
        args ? args->Version : 0, args ? PipelineLevel(args->Flags) : "-", (unsigned long)hr);
    return hr;
}

// Wraps the two adapter entries the runtime calls next, in the table the runtime owns, after the UMD succeeded.
void WrapAdapter(D3D10DDIARG_OPENADAPTER *args) {
    if (!args || !args->pAdapterFuncs) return;
    if (args->pAdapterFuncs->pfnCreateDevice && args->pAdapterFuncs->pfnCreateDevice != TraceCreateDevice) {
        g_realCreateDevice = args->pAdapterFuncs->pfnCreateDevice;
        args->pAdapterFuncs->pfnCreateDevice = TraceCreateDevice;
    }
    if (args->pAdapterFuncs->pfnCalcPrivateDeviceSize &&
        args->pAdapterFuncs->pfnCalcPrivateDeviceSize != TraceCalcSize) {
        g_realCalcSize = args->pAdapterFuncs->pfnCalcPrivateDeviceSize;
        args->pAdapterFuncs->pfnCalcPrivateDeviceSize = TraceCalcSize;
    }
}

HRESULT APIENTRY TraceOpen10(D3D10DDIARG_OPENADAPTER *args) {
    const HRESULT hr = g_realOpen10(args);
    Say("entry: OpenAdapter10 interface %08X version %08X -> 0x%08lX\n", args ? args->Interface : 0,
        args ? args->Version : 0, (unsigned long)hr);
    if (SUCCEEDED(hr)) WrapAdapter(args);
    return hr;
}

HRESULT APIENTRY TraceOpen102(D3D10DDIARG_OPENADAPTER *args) {
    const HRESULT hr = g_realOpen102(args);
    Say("entry: OpenAdapter10_2 interface %08X version %08X -> 0x%08lX\n", args ? args->Interface : 0,
        args ? args->Version : 0, (unsigned long)hr);
    if (SUCCEEDED(hr)) WrapAdapter(args);
    return hr;
}

FARPROC WINAPI TraceGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC p = g_realGetProcAddress(module, name);
    if ((ULONG_PTR)name <= 0xFFFF || strncmp(name, "OpenAdapter", 11)) return p;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    Say("entry: the runtime asks %s for %s: %s\n", path, name, p ? "exported" : "not exported");
    if (p && !strcmp(name, "OpenAdapter10")) {
        g_realOpen10 = (OpenAdapterFn)p;
        return (FARPROC)TraceOpen10;
    }
    if (p && !strcmp(name, "OpenAdapter10_2")) {
        g_realOpen102 = (OpenAdapterFn)p;
        return (FARPROC)TraceOpen102;
    }
    return p;
}

// Replaces every by-name GetProcAddress import of one loaded module. Returns the count.
int PatchImports(const char *moduleName) {
    HMODULE m = GetModuleHandleA(moduleName);
    if (!m) return 0;
    BYTE *base = (BYTE *)m;
    auto nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name,
                       "GetProcAddress"))
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
            if (!g_realGetProcAddress) g_realGetProcAddress = (GetProcAddressFn)slots->u1.Function;
            slots->u1.Function = (ULONG_PTR)TraceGetProcAddress;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            ++patched;
        }
    }
    return patched;
}

void InstallEntryTrace() {
    const char *runtimes[] = {"d3d10.dll", "d3d10core.dll", "d3d10_1.dll", "d3d10_1core.dll", "d3d11.dll", "dxgi.dll"};
    for (const char *r : runtimes) LoadLibraryA(r);
    int total = 0;
    for (const char *r : runtimes) total += PatchImports(r);
    if (!g_realGetProcAddress) g_realGetProcAddress = GetProcAddress;
    Say("entry trace: %d GetProcAddress imports of the runtime DLLs replaced\n", total);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

HWND MakeWindow(bool show) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3d10probe";
    RegisterClassW(&wc);
    RECT r = {0, 0, (LONG)kSize, (LONG)kSize};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"d3d10probe", WS_OVERLAPPEDWINDOW, 64, 64, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd && show) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

template <class T> void Release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

const char kShader[] =
    "struct V { float2 pos : POSITION; };\n"
    "float4 vs(V v) : SV_Position { return float4(v.pos, 0.5, 1.0); }\n"
    "float4 ps() : SV_Target { return float4(240.0 / 255.0, 160.0 / 255.0, 16.0 / 255.0, 1.0); }\n";

ID3D10Blob *Compile(pD3DCompile compile, const char *entry, const char *target) {
    ID3D10Blob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "d3d10probe", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        Say("D3DCompile(%s, %s) -> 0x%08lX %s\n", entry, target, (unsigned long)hr,
            errors ? (const char *)errors->GetBufferPointer() : "");
        Release(code);
    }
    Release(errors);
    return code;
}

struct Options {
    D3D10_DRIVER_TYPE type = D3D10_DRIVER_TYPE_HARDWARE;
    const char *software = nullptr;  // --software <dll>: D3D10_DRIVER_TYPE_SOFTWARE with this module
    int frames = 60;
    bool show = false;
    bool debug = false;
    bool traceEntry = false;
};

const char *TypeName(D3D10_DRIVER_TYPE t) {
    switch (t) {
    case D3D10_DRIVER_TYPE_HARDWARE: return "hardware";
    case D3D10_DRIVER_TYPE_REFERENCE: return "reference";
    case D3D10_DRIVER_TYPE_NULL: return "null";
    case D3D10_DRIVER_TYPE_SOFTWARE: return "software";
    case D3D10_DRIVER_TYPE_WARP: return "warp";
    }
    return "unknown";
}

int Run(const Options &o) {
    HMODULE compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compilerDll ? (pD3DCompile)GetProcAddress(compilerDll, "D3DCompile") : nullptr;
    if (!compile) {
        Say("d3dcompiler_47.dll: D3DCompile not available (%lu)\n", GetLastError());
        return 1;
    }
    HMODULE software = nullptr;
    if (o.type == D3D10_DRIVER_TYPE_SOFTWARE) {
        software = LoadLibraryA(o.software);
        if (!software) {
            Say("software module %s: not loaded (%lu)\n", o.software, GetLastError());
            return 1;
        }
    }
    if (o.traceEntry) InstallEntryTrace();
    HWND hwnd = MakeWindow(o.show);
    if (!hwnd) {
        Say("window: not created (%lu)\n", GetLastError());
        return 1;
    }
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = kSize;
    sd.BufferDesc.Height = kSize;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D10Device *dev = nullptr;
    IDXGISwapChain *chain = nullptr;
    const UINT flags = o.debug ? D3D10_CREATE_DEVICE_DEBUG : 0;
    HRESULT hr = D3D10CreateDeviceAndSwapChain(nullptr, o.type, software, flags, D3D10_SDK_VERSION, &sd, &chain, &dev);
    Say("D3D10CreateDeviceAndSwapChain(%s) -> 0x%08lX\n", TypeName(o.type), (unsigned long)hr);
    if (FAILED(hr) || !dev) {
        // Without the swap chain, so that a refusal of the DXGI half and a refusal of the device can be told apart.
        hr = D3D10CreateDevice(nullptr, o.type, software, flags, D3D10_SDK_VERSION, &dev);
        Say("D3D10CreateDevice(%s) -> 0x%08lX\n", TypeName(o.type), (unsigned long)hr);
        Modules();
        Release(dev);
        DestroyWindow(hwnd);
        Say("served by: %s\n", ServedBy());
        Say("verdict NO-DEVICE\n");
        return 2;
    }
    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(adapter->GetDesc(&ad)))
            Say("adapter \"%ls\" vendor 0x%04X device 0x%04X luid %08lX:%08lX dedicated %zu MiB shared %zu MiB\n",
                ad.Description, ad.VendorId, ad.DeviceId, (unsigned long)ad.AdapterLuid.HighPart,
                (unsigned long)ad.AdapterLuid.LowPart, ad.DedicatedVideoMemory >> 20, ad.SharedSystemMemory >> 20);
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(ID3D10Device), &umd)))
            Say("UMD version %u.%u.%u.%u\n", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                LOWORD(umd.LowPart));
    }
    Release(adapter);
    Release(dxgiDevice);

    int rc = 0;
    ID3D10Blob *vsCode = Compile(compile, "vs", "vs_4_0"), *psCode = Compile(compile, "ps", "ps_4_0");
    ID3D10VertexShader *vs = nullptr;
    ID3D10PixelShader *ps = nullptr;
    ID3D10InputLayout *layout = nullptr;
    ID3D10Buffer *vb = nullptr;
    ID3D10Texture2D *target = nullptr, *staging = nullptr, *back = nullptr;
    ID3D10RenderTargetView *rtv = nullptr, *backView = nullptr;
    ID3D10RasterizerState *raster = nullptr;
    do {
        if (!vsCode || !psCode) { rc = 1; break; }
        hr = dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &vs);
        if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), &ps);
        const D3D10_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                                  D3D10_INPUT_PER_VERTEX_DATA, 0};
        if (SUCCEEDED(hr))
            hr = dev->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout);
        Say("shaders and input layout -> 0x%08lX\n", (unsigned long)hr);
        if (FAILED(hr)) { rc = 1; break; }
        // The right triangle over the top-left half: (-1,1) (1,1) (-1,-1) in clip space.
        const float verts[6] = {-1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f};
        D3D10_BUFFER_DESC bd = {sizeof(verts), D3D10_USAGE_IMMUTABLE, D3D10_BIND_VERTEX_BUFFER, 0, 0};
        D3D10_SUBRESOURCE_DATA init = {verts, 0, 0};
        hr = dev->CreateBuffer(&bd, &init, &vb);
        D3D10_TEXTURE2D_DESC td = {};
        td.Width = td.Height = kSize;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D10_USAGE_DEFAULT;
        td.BindFlags = D3D10_BIND_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, nullptr, &target);
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(target, nullptr, &rtv);
        td.Usage = D3D10_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, nullptr, &staging);
        D3D10_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D10_FILL_SOLID;
        rd.CullMode = D3D10_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &raster);
        Say("resources -> 0x%08lX\n", (unsigned long)hr);
        if (FAILED(hr)) { rc = 1; break; }

        const float clear[4] = {kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, 1.0f};
        const UINT stride = 2 * sizeof(float), offset = 0;
        D3D10_VIEWPORT vp = {0, 0, kSize, kSize, 0.0f, 1.0f};
        auto draw = [&](ID3D10RenderTargetView *view) {
            dev->OMSetRenderTargets(1, &view, nullptr);
            dev->RSSetViewports(1, &vp);
            dev->RSSetState(raster);
            dev->ClearRenderTargetView(view, clear);
            dev->IASetInputLayout(layout);
            dev->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            dev->VSSetShader(vs);
            dev->GSSetShader(nullptr);
            dev->PSSetShader(ps);
            dev->Draw(3, 0);
        };
        draw(rtv);
        dev->CopyResource(staging, target);
        D3D10_MAPPED_TEXTURE2D map = {};
        hr = staging->Map(0, D3D10_MAP_READ, 0, &map);
        Say("Map -> 0x%08lX\n", (unsigned long)hr);
        int bad = 0;
        unsigned triCount = 0, clearCount = 0, other = 0;
        if (SUCCEEDED(hr)) {
            auto px = [&](UINT x, UINT y) { return (const BYTE *)map.pData + y * map.RowPitch + x * 4; };
            for (UINT y = 0; y < kSize; y++)
                for (UINT x = 0; x < kSize; x++) {
                    const BYTE *p = px(x, y);
                    if (!memcmp(p, kTri, 4)) triCount++;
                    else if (!memcmp(p, kClear, 4)) clearCount++;
                    else if (other++ < 4) Say("pixel (%u,%u) = %02X %02X %02X %02X, neither colour\n", x, y, p[0], p[1], p[2], p[3]);
                }
            const struct { UINT x, y; const BYTE *want; } samples[] = {
                {2, 2, kTri}, {100, 20, kTri}, {20, 200, kTri}, {120, 120, kTri},
                {250, 250, kClear}, {200, 100, kClear}, {140, 140, kClear}, {255, 1, kClear}};
            for (auto &s : samples) {
                const BYTE *p = px(s.x, s.y);
                if (memcmp(p, s.want, 4)) {
                    Say("pixel (%u,%u) = %02X %02X %02X %02X, want %02X %02X %02X %02X\n", s.x, s.y, p[0], p[1], p[2],
                        p[3], s.want[0], s.want[1], s.want[2], s.want[3]);
                    bad++;
                }
            }
            staging->Unmap(0);
        } else {
            bad = 1;
        }
        const unsigned wantTri = kSize * (kSize - 1) / 2;
        Say("pixels: triangle %u (exact rule %u), clear %u, other %u, wrong samples %d\n", triCount, wantTri,
            clearCount, other, bad);
        const bool pixelsOk = bad == 0 && other == 0 && triCount == wantTri && clearCount == kSize * kSize - wantTri;
        if (!pixelsOk) rc = 3;

        // The presentation path: the same frame into the swap chain's buffer, then Present.
        unsigned presented = 0, occluded = 0, failed = 0;
        HRESULT firstFailure = S_OK;
        if (o.frames > 0) {
            hr = chain->GetBuffer(0, __uuidof(ID3D10Texture2D), (void **)&back);
            if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(back, nullptr, &backView);
            Say("swap chain buffer -> 0x%08lX\n", (unsigned long)hr);
            if (FAILED(hr)) { if (!rc) rc = 4; break; }
            for (int f = 0; f < o.frames; f++) {
                Pump();
                draw(backView);
                hr = chain->Present(0, 0);
                if (hr == DXGI_STATUS_OCCLUDED) occluded++;
                else if (FAILED(hr)) { if (!failed++) firstFailure = hr; }
                else presented++;
            }
            Say("present: %d frames, %u presented, %u occluded, %u failed (first 0x%08lX), window %s\n", o.frames,
                presented, occluded, failed, (unsigned long)firstFailure, o.show ? "shown" : "hidden");
            if (failed && !rc) rc = 4;
        }
    } while (false);

    dev->ClearState();
    dev->Flush();
    Modules();
    Say("served by: %s\n", ServedBy());
    Release(backView);
    Release(back);
    Release(raster);
    Release(staging);
    Release(rtv);
    Release(target);
    Release(vb);
    Release(layout);
    Release(ps);
    Release(vs);
    Release(psCode);
    Release(vsCode);
    Release(chain);
    Release(dev);
    DestroyWindow(hwnd);
    Say("verdict %s\n", rc == 0 ? "PASS" : rc == 3 ? "FAIL-PIXELS" : rc == 4 ? "FAIL-PRESENT" : "PROBE-ERROR");
    return rc;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    const char *out = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            printf("d3d10probe [--driver hardware|warp|reference] [--software <dll>] [--frames N] [--show] [--debug]"
                   " [--trace-entry]"
                   " [--out file]\n"
                   "exit 0 PASS, 3 wrong pixels, 4 a Present failed, 2 no device, 1 usage or probe error\n");
            return 0;
        } else if (!strcmp(argv[i], "--driver") && i + 1 < argc) {
            const char *d = argv[++i];
            if (!strcmp(d, "hardware")) o.type = D3D10_DRIVER_TYPE_HARDWARE;
            else if (!strcmp(d, "warp")) o.type = D3D10_DRIVER_TYPE_WARP;
            else if (!strcmp(d, "reference")) o.type = D3D10_DRIVER_TYPE_REFERENCE;
            else {
                fprintf(stderr, "unknown driver %s\n", d);
                return 1;
            }
        } else if (!strcmp(argv[i], "--software") && i + 1 < argc) {
            o.type = D3D10_DRIVER_TYPE_SOFTWARE;
            o.software = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            o.frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--show")) {
            o.show = true;
        } else if (!strcmp(argv[i], "--debug")) {
            o.debug = true;
        } else if (!strcmp(argv[i], "--trace-entry")) {
            o.traceEntry = true;
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else {
            fprintf(stderr, "unknown argument %s (try --help)\n", argv[i]);
            return 1;
        }
    }
    if (out) {
        if (fopen_s(&g_out, out, "w") != 0 || !g_out) g_out = stdout;
    }
    Say("d3d10probe pid %lu, %s build\n", GetCurrentProcessId(), sizeof(void *) == 8 ? "x64" : "x86");
    int rc = Run(o);
    if (g_out != stdout) fclose(g_out);
    return rc;
}
