// d3d9probe: which path a Direct3D 9 application takes on this adapter, and whether it renders correctly.
//
// Two modes:
//   --mode default   Direct3DCreate9Ex, the path every D3D9 application takes
//   --mode on12      Direct3DCreate9On12Ex with Enable9On12 = TRUE, the D3D9-on-D3D12 mapping layer forced on
//
// For each mode the probe prints the adapter identifier (the Driver field names the user-mode driver the runtime
// reports), whether the device answers IID_IDirect3DDevice9On12 (that is, runs on the mapping layer), the loaded
// modules that name a graphics stack, then renders with the fixed-function pipeline into a 256x256 render target:
// a clear and one pre-transformed triangle, read back with GetRenderTargetData. The check is exact on sample pixels
// away from the triangle's edge and on the count of triangle pixels (the top-left fill rule gives 32 896 for this
// triangle). Then --frames frames of --draws triangle draws each, closed by an event query, give draws per second.
// No window is shown: the device runs on a hidden window. Exit code 0 = rendered correctly, 3 = wrong pixels,
// 2 = no device, 1 = usage.
#include <windows.h>
#include <d3d9.h>
#include <initguid.h>  // defines IID_IDirect3DDevice9On12 here, there is no import library for it
#include <d3d9on12.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <chrono>

namespace {

const UINT kSize = 256;
const D3DCOLOR kClear = D3DCOLOR_XRGB(0x20, 0x40, 0x80);
const D3DCOLOR kTri = D3DCOLOR_XRGB(0xF0, 0xA0, 0x10);

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};

FILE *g_out = stdout;

void Say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (g_out != stdout) {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
    }
}

void Modules() {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    const char *keys[] = {"d3d9", "d3d12", "d3d11", "d3d10", "dxgi", "bc250", "amdgpu", "dxvk", "vkd3d", "vulkan",
                          "radv", "zink", "lvp", "opengl", "warp"};
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
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

HWND HiddenWindow() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"d3d9probe";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"d3d9probe", WS_OVERLAPPEDWINDOW, 0, 0, kSize, kSize, nullptr,
                           nullptr, wc.hInstance, nullptr);
}

int Run(bool on12, int frames, int draws) {
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9) {
        Say("d3d9.dll: not loaded (%lu)\n", GetLastError());
        return 2;
    }
    IDirect3D9Ex *d3d = nullptr;
    HRESULT hr;
    if (on12) {
        auto create = (PFN_Direct3DCreate9On12Ex)GetProcAddress(d3d9, "Direct3DCreate9On12Ex");
        if (!create) {
            Say("Direct3DCreate9On12Ex: not exported\n");
            return 2;
        }
        D3D9ON12_ARGS args = {};
        args.Enable9On12 = TRUE;
        hr = create(D3D_SDK_VERSION, &args, 1, &d3d);
        Say("Direct3DCreate9On12Ex -> 0x%08lX\n", (unsigned long)hr);
    } else {
        typedef HRESULT(WINAPI * PFN_Create9Ex)(UINT, IDirect3D9Ex **);
        auto create = (PFN_Create9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");
        if (!create) {
            Say("Direct3DCreate9Ex: not exported\n");
            return 2;
        }
        hr = create(D3D_SDK_VERSION, &d3d);
        Say("Direct3DCreate9Ex -> 0x%08lX\n", (unsigned long)hr);
    }
    if (FAILED(hr) || !d3d) return 2;

    UINT count = d3d->GetAdapterCount();
    Say("adapters %u\n", count);
    for (UINT a = 0; a < count; a++) {
        D3DADAPTER_IDENTIFIER9 id = {};
        if (SUCCEEDED(d3d->GetAdapterIdentifier(a, 0, &id))) {
            Say("adapter %u: \"%s\" driver \"%s\" version %u.%u.%u.%u vendor 0x%04lX device 0x%04lX\n", a,
                id.Description, id.Driver, HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
                HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart), id.VendorId, id.DeviceId);
        }
    }
    D3DCAPS9 caps = {};
    if (SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) {
        Say("caps: vs %lu.%lu ps %lu.%lu, max texture %lux%lu, simultaneous RTs %lu\n",
            D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
            D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
            caps.MaxTextureWidth, caps.MaxTextureHeight, caps.NumSimultaneousRTs);
    }

    HWND hwnd = HiddenWindow();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = kSize;
    pp.BackBufferHeight = kSize;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice9Ex *dev = nullptr;
    const DWORD vpModes[2] = {D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING};
    for (DWORD vp : vpModes) {
        hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, vp | D3DCREATE_FPU_PRESERVE, &pp,
                                 nullptr, &dev);
        Say("CreateDeviceEx(%s) -> 0x%08lX\n", vp == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware vp" : "software vp",
            (unsigned long)hr);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr) || !dev) {
        Modules();
        d3d->Release();
        return 2;
    }
    IUnknown *on12dev = nullptr;
    HRESULT qi = dev->QueryInterface(IID_IDirect3DDevice9On12, (void **)&on12dev);
    Say("device on the 9On12 layer: %s (QueryInterface 0x%08lX)\n", SUCCEEDED(qi) ? "yes" : "no", (unsigned long)qi);
    if (on12dev) on12dev->Release();

    IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
    hr = dev->CreateRenderTarget(kSize, kSize, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    if (SUCCEEDED(hr))
        hr = dev->CreateOffscreenPlainSurface(kSize, kSize, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (FAILED(hr)) {
        Say("surfaces -> 0x%08lX\n", (unsigned long)hr);
        return 2;
    }
    dev->SetRenderTarget(0, rt);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    // Right triangle over the top-left half: pixel centres (x + 0.5, y + 0.5) with x + y < 256 are inside, 32 896 of
    // them; the diagonal itself (x + y = 255.5 + 0.5) falls on the edge and the top-left rule leaves it out.
    const Vertex tri[3] = {{0.0f, 0.0f, 0.5f, 1.0f, kTri},
                           {(float)kSize, 0.0f, 0.5f, 1.0f, kTri},
                           {0.0f, (float)kSize, 0.5f, 1.0f, kTri}};

    dev->BeginScene();
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    dev->EndScene();
    hr = dev->GetRenderTargetData(rt, sys);
    Say("GetRenderTargetData -> 0x%08lX\n", (unsigned long)hr);
    int bad = 0;
    unsigned triCount = 0, clearCount = 0, other = 0;
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(hr) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        for (UINT y = 0; y < kSize; y++) {
            const DWORD *row = (const DWORD *)((const BYTE *)lr.pBits + y * lr.Pitch);
            for (UINT x = 0; x < kSize; x++) {
                DWORD p = row[x] & 0x00FFFFFF;
                if (p == (kTri & 0x00FFFFFF)) triCount++;
                else if (p == (kClear & 0x00FFFFFF)) clearCount++;
                else other++;
            }
        }
        const struct { UINT x, y; D3DCOLOR want; } samples[] = {
            {2, 2, kTri}, {100, 20, kTri}, {20, 200, kTri}, {120, 120, kTri},
            {250, 250, kClear}, {200, 100, kClear}, {140, 140, kClear}, {255, 1, kClear}};
        for (auto &s : samples) {
            DWORD p = ((const DWORD *)((const BYTE *)lr.pBits + s.y * lr.Pitch))[s.x] & 0x00FFFFFF;
            if (p != (s.want & 0x00FFFFFF)) {
                Say("pixel (%u,%u) = 0x%06lX, want 0x%06lX\n", s.x, s.y, p, s.want & 0x00FFFFFF);
                bad++;
            }
        }
        sys->UnlockRect();
    } else {
        bad = 1;
    }
    const unsigned wantTri = kSize * (kSize + 1) / 2;
    Say("pixels: triangle %u (exact rule %u), clear %u, other %u, wrong samples %d\n", triCount, wantTri, clearCount,
        other, bad);
    bool ok = bad == 0 && other == 0 && triCount >= wantTri - kSize && triCount <= wantTri + kSize;

    IDirect3DQuery9 *q = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
    auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; f++) {
        dev->BeginScene();
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0);
        for (int d = 0; d < draws; d++) dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
        dev->EndScene();
        if (q) {
            q->Issue(D3DISSUE_END);
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            }
        }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (frames > 0)
        Say("timing: %d frames x %d draws in %.3f s = %.1f frames/s, %.0f draws/s\n", frames, draws, s, frames / s,
            (double)frames * draws / s);
    Modules();
    if (q) q->Release();
    sys->Release();
    rt->Release();
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    Say("verdict %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 3;
}

}  // namespace

int main(int argc, char **argv) {
    bool on12 = false;
    int frames = 200, draws = 100;
    const char *out = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            printf("d3d9probe [--mode default|on12] [--frames N] [--draws N] [--out file]\n");
            return 0;
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "on12")) on12 = true;
            else if (strcmp(m, "default")) {
                fprintf(stderr, "unknown mode %s\n", m);
                return 1;
            }
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--draws") && i + 1 < argc) {
            draws = atoi(argv[++i]);
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
    Say("d3d9probe mode %s pid %lu\n", on12 ? "on12" : "default", GetCurrentProcessId());
    int rc = Run(on12, frames, draws);
    if (g_out != stdout) fclose(g_out);
    return rc;
}
