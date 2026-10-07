// Which OpenAdapter export does each Direct3D runtime ask the registered UMD for, and with which interface does
// it create the device? Patches the GetProcAddress import of the runtime DLLs in this process, wraps OpenAdapter10,
// OpenAdapter10_2, CalcPrivateDeviceSize and CreateDevice, logs and calls through. No window.
//   entryspy.exe show|hide102 all|d3d10
// hide102: GetProcAddress("OpenAdapter10_2") answers NULL, as for a UMD without that export.
#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)
#include <d3d10.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d10.lib")
#pragma comment(lib, "d3d10_1.lib")
#pragma comment(lib, "d3d11.lib")

typedef HRESULT(APIENTRY *OpenFn)(D3D10DDIARG_OPENADAPTER *);
static OpenFn Real10, Real102;
typedef FARPROC(WINAPI *GpaFn)(HMODULE, LPCSTR);
static GpaFn RealGpa;
static bool HideOa102;
static PFND3D10DDI_CREATEDEVICE RealCreate;
static PFND3D10DDI_CALCPRIVATEDEVICESIZE RealSize;

static HRESULT APIENTRY SpyCreate(D3D10DDI_HADAPTER h, D3D10DDIARG_CREATEDEVICE *c)
{
    printf("  CreateDevice interface=%08x version=%08x\n", c ? c->Interface : 0, c ? c->Version : 0);
    return RealCreate(h, c);
}
static SIZE_T APIENTRY SpySize(D3D10DDI_HADAPTER h, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *c)
{
    printf("  CalcPrivateDeviceSize interface=%08x version=%08x\n", c ? c->Interface : 0, c ? c->Version : 0);
    return RealSize(h, c);
}
static void Wrap(D3D10DDIARG_OPENADAPTER *a)
{
    if (!a || !a->pAdapterFuncs) return;
    if (a->pAdapterFuncs->pfnCreateDevice != SpyCreate) {
        RealCreate = a->pAdapterFuncs->pfnCreateDevice;
        a->pAdapterFuncs->pfnCreateDevice = SpyCreate;
    }
    if (a->pAdapterFuncs->pfnCalcPrivateDeviceSize != SpySize) {
        RealSize = a->pAdapterFuncs->pfnCalcPrivateDeviceSize;
        a->pAdapterFuncs->pfnCalcPrivateDeviceSize = SpySize;
    }
}
static HRESULT APIENTRY Spy10(D3D10DDIARG_OPENADAPTER *a)
{
    printf("  call OpenAdapter10 interface=%08x version=%08x\n", a ? a->Interface : 0, a ? a->Version : 0);
    HRESULT hr = Real10(a);
    printf("  OpenAdapter10 -> %08lx\n", hr);
    if (SUCCEEDED(hr)) Wrap(a);
    return hr;
}
static HRESULT APIENTRY Spy102(D3D10DDIARG_OPENADAPTER *a)
{
    printf("  call OpenAdapter10_2 interface=%08x version=%08x\n", a ? a->Interface : 0, a ? a->Version : 0);
    HRESULT hr = Real102(a);
    printf("  OpenAdapter10_2 -> %08lx\n", hr);
    if (SUCCEEDED(hr)) Wrap(a);
    return hr;
}

static FARPROC WINAPI HookGpa(HMODULE m, LPCSTR name)
{
    FARPROC p = RealGpa(m, name);
    if ((ULONG_PTR)name > 0xFFFF && !strncmp(name, "OpenAdapter", 11)) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        printf("  GetProcAddress(%s, %s) -> %p\n", strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path, name, (void *)p);
        if (p && !strcmp(name, "OpenAdapter10")) { Real10 = (OpenFn)p; return (FARPROC)Spy10; }
        if (p && !strcmp(name, "OpenAdapter10_2")) {
            if (HideOa102) { printf("  (hidden)\n"); return nullptr; }
            Real102 = (OpenFn)p;
            return (FARPROC)Spy102;
        }
    }
    return p;
}

static int Patch(const char *module)
{
    HMODULE m = GetModuleHandleA(module);
    if (!m) return 0;
    auto dos = (IMAGE_DOS_HEADER *)m;
    auto nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    if (!dir.VirtualAddress) return 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)m + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA *)((BYTE *)m + imp->OriginalFirstThunk);
        auto iat = (IMAGE_THUNK_DATA *)((BYTE *)m + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME *)((BYTE *)m + names->u1.AddressOfData);
            if (strcmp((const char *)byName->Name, "GetProcAddress")) continue;
            DWORD old = 0;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            if (!RealGpa) RealGpa = (GpaFn)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)HookGpa;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            ++patched;
        }
    }
    printf("patched %d GetProcAddress imports in %s\n", patched, module);
    return patched;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    HideOa102 = argc > 1 && !strcmp(argv[1], "hide102");
    const bool all = !(argc > 2 && !strcmp(argv[2], "d3d10"));
    const char *mods[] = {"d3d10.dll", "d3d10core.dll", "d3d10_1.dll", "d3d10_1core.dll", "d3d11.dll", "dxgi.dll"};
    for (const char *m : mods) LoadLibraryA(m);
    for (const char *m : mods) Patch(m);
    if (!RealGpa) RealGpa = GetProcAddress;

    printf("D3D10CreateDevice (the D3D10.0 runtime)\n");
    ID3D10Device *d10 = nullptr;
    HRESULT hr = D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION, &d10);
    printf("  -> %08lx\n", hr);
    if (d10) d10->Release();
    if (!all) return 0;

    printf("D3D10CreateDevice1 level 10_0 (the D3D10.1 runtime)\n");
    ID3D10Device1 *d101 = nullptr;
    hr = D3D10CreateDevice1(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_0,
                            D3D10_1_SDK_VERSION, &d101);
    printf("  -> %08lx\n", hr);
    if (d101) d101->Release();

    printf("D3D11CreateDevice (the D3D11 runtime)\n");
    ID3D11Device *d11 = nullptr;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr,
                           nullptr);
    printf("  -> %08lx\n", hr);
    if (d11) d11->Release();
    return 0;
}
