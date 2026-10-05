// SPDX-License-Identifier: MIT
// shadowtest: headless experiment for DXGI module shadowing (M16 WSI-over-DXGI design).
//
// Question: when an application-local dxgi.dll (DXVK, per-app vkd3d-proton route) is already loaded,
// does System32 d3d11.dll - loaded by full path, as Mesa's util_load_system_library does - bind its
// dxgi.dll import to the System32 module or to the app-local one, and does a system D3D11 device work?
// Also probes candidate mitigations: IAT rebinding, activation-context redirection, search flags.
//
// No window, no swap chain, no resident process. All D3D/DXGI/DComp entry points are resolved with
// GetProcAddress so that this executable itself imports none of those modules.
//
// Usage: shadowtest.exe <mode>
//   control         no app-local dxgi.dll is loaded
//   shadow          app-local dxgi.dll by name first, then System32 dxgi/d3d11 by full path
//   shadow-rebind   shadow + rewrite d3d11 IAT and dcomp delay IAT entries to the System32 modules
//   shadow-actctx   shadow + load d3d11 inside an activation context with <file loadFrom=System32>
//   shadow-search   shadow + LoadLibraryExW("d3d11.dll", LOAD_LIBRARY_SEARCH_SYSTEM32)
//   sysfirst        System32 dxgi by full path first, then LoadLibraryW("dxgi.dll") by name
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

std::string narrow(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Driver Store folder names are machine-specific; keep the class and file name only.
std::string describe_path(const std::wstring &path)
{
    std::string p = narrow(path);
    std::string lower = p;
    for (auto &c : lower) c = (char)tolower((unsigned char)c);
    size_t ds = lower.find("\\driverstore\\");
    if (ds != std::string::npos) {
        size_t slash = p.find_last_of('\\');
        return "<DriverStore>\\...\\" + p.substr(slash + 1);
    }
    return p;
}

std::string module_path(HMODULE m)
{
    if (!m) return "(null)";
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(m, buf, 1024);
    return describe_path(std::wstring(buf, n));
}

std::string owner_of(const void *p)
{
    if (!p) return "(null)";
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(p), &m))
        return "(no module)";
    return module_path(m);
}

// Module that implements a COM object's methods: owner of vtable slot 3 (first non-IUnknown method).
std::string vtbl_owner(IUnknown *o)
{
    if (!o) return "(null)";
    void **vt = *reinterpret_cast<void ***>(o);
    return owner_of(vt[3]);
}

std::wstring system_path(const wchar_t *name)
{
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring p(dir, n);
    p += L"\\";
    p += name;
    return p;
}

std::wstring exe_dir()
{
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(nullptr, buf, 1024);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L'\\'));
}

struct ImportEntry {
    std::string name;
    WORD ordinal = 0;
    bool by_ordinal = false;
    void **slot = nullptr;
};

IMAGE_NT_HEADERS64 *nt_of(HMODULE mod)
{
    auto *base = reinterpret_cast<BYTE *>(mod);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    return reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
}

std::vector<ImportEntry> static_imports(HMODULE mod, const char *dll)
{
    std::vector<ImportEntry> out;
    auto *base = reinterpret_cast<BYTE *>(mod);
    const auto &dir = nt_of(mod)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return out;
    for (auto *d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char *>(base + d->Name), dll)) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto *iat = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            ImportEntry e;
            e.slot = reinterpret_cast<void **>(&iat->u1.Function);
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                e.by_ordinal = true;
                e.ordinal = (WORD)IMAGE_ORDINAL64(names->u1.Ordinal);
            } else {
                e.name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name;
            }
            out.push_back(e);
        }
    }
    return out;
}

struct DelayImports {
    bool found = false;
    HMODULE *module_slot = nullptr;
    std::vector<ImportEntry> entries;
};

DelayImports delay_imports(HMODULE mod, const char *dll)
{
    DelayImports out;
    auto *base = reinterpret_cast<BYTE *>(mod);
    const auto &dir = nt_of(mod)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return out;
    for (auto *d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR *>(base + dir.VirtualAddress); d->DllNameRVA; ++d) {
        if (_stricmp(reinterpret_cast<const char *>(base + d->DllNameRVA), dll)) continue;
        out.found = true;
        out.module_slot = reinterpret_cast<HMODULE *>(base + d->ModuleHandleRVA);
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + d->ImportNameTableRVA);
        auto *iat = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            ImportEntry e;
            e.slot = reinterpret_cast<void **>(&iat->u1.Function);
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                e.by_ordinal = true;
                e.ordinal = (WORD)IMAGE_ORDINAL64(names->u1.Ordinal);
            } else {
                e.name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name;
            }
            out.entries.push_back(e);
        }
    }
    return out;
}

std::string entry_name(const ImportEntry &e)
{
    return e.by_ordinal ? ("#" + std::to_string(e.ordinal)) : e.name;
}

void report_static(const char *who, HMODULE mod, const char *dll)
{
    auto list = static_imports(mod, dll);
    say("  [%s] static imports from %s: %zu\n", who, dll, list.size());
    for (const auto &e : list)
        say("    %-28s -> %s\n", entry_name(e).c_str(), owner_of(*e.slot).c_str());
}

void report_delay(const char *who, HMODULE mod, const char *dll)
{
    auto d = delay_imports(mod, dll);
    if (!d.found) {
        say("  [%s] no delay imports from %s\n", who, dll);
        return;
    }
    say("  [%s] delay imports from %s: %zu, module slot -> %s\n", who, dll, d.entries.size(),
        (d.module_slot && *d.module_slot) ? module_path(*d.module_slot).c_str() : "(unresolved)");
    for (const auto &e : d.entries)
        say("    %-28s -> %s\n", entry_name(e).c_str(), owner_of(*e.slot).c_str());
}

bool write_slot(void **slot, void *value)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *slot = value;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return true;
}

void *resolve(HMODULE target, const ImportEntry &e)
{
    return e.by_ordinal ? reinterpret_cast<void *>(GetProcAddress(target, MAKEINTRESOURCEA(e.ordinal)))
                        : reinterpret_cast<void *>(GetProcAddress(target, e.name.c_str()));
}

// Rewrites every entry of 'dll' in mod's static IAT that does not already point into 'target'.
int rebind_static(HMODULE mod, const char *dll, HMODULE target)
{
    int changed = 0;
    for (const auto &e : static_imports(mod, dll)) {
        void *want = resolve(target, e);
        if (!want) {
            say("    rebind: %s missing in target\n", entry_name(e).c_str());
            continue;
        }
        if (*e.slot != want && write_slot(e.slot, want)) ++changed;
    }
    return changed;
}

// Pre-resolves a delay-import descriptor against 'target' so the delay helper never runs its
// LoadLibraryExA-by-name path for it.
int rebind_delay(HMODULE mod, const char *dll, HMODULE target)
{
    auto d = delay_imports(mod, dll);
    if (!d.found) return 0;
    int changed = 0;
    for (const auto &e : d.entries) {
        void *want = resolve(target, e);
        if (!want) {
            say("    rebind-delay: %s missing in target\n", entry_name(e).c_str());
            continue;
        }
        if (*e.slot != want && write_slot(e.slot, want)) ++changed;
    }
    if (d.module_slot && *d.module_slot != target) {
        write_slot(reinterpret_cast<void **>(d.module_slot), target);
        ++changed;
    }
    return changed;
}

void list_modules(const char *title)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(GetCurrentProcess(), mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return;
    size_t n = needed / sizeof(HMODULE);
    say("  [%s] %zu modules; graphics-related:\n", title, n);
    for (size_t i = 0; i < n && i < 1024; ++i) {
        wchar_t base[MAX_PATH];
        GetModuleBaseNameW(GetCurrentProcess(), mods[i], base, MAX_PATH);
        std::wstring b = base;
        for (auto &c : b) c = (wchar_t)towlower(c);
        if (b.rfind(L"d3d", 0) == 0 || b.rfind(L"dxgi", 0) == 0 || b.rfind(L"dcomp", 0) == 0 ||
            b.rfind(L"vulkan", 0) == 0 || b.rfind(L"nv", 0) == 0 || b.rfind(L"dxcore", 0) == 0)
            say("    %p %s\n", (void *)mods[i], module_path(mods[i]).c_str());
    }
}

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);
typedef HRESULT(WINAPI *PFN_DCompositionCreateDevice)(IDXGIDevice *, REFIID, void **);

// SEH wrapper: a crash inside the runtime is a result, not the end of the experiment.
HRESULT seh_create_device(PFN_D3D11_CREATE_DEVICE fn, IDXGIAdapter *adapter, D3D_DRIVER_TYPE type,
                          ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context,
                          DWORD *exc_out)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    *exc_out = 0;
    __try {
        return fn(adapter, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, device, level, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc_out = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_create_factory(PFN_CreateDXGIFactory2 fn, IDXGIFactory1 **factory, DWORD *exc_out)
{
    *exc_out = 0;
    __try {
        return fn(0, __uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc_out = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_dcomp(PFN_DCompositionCreateDevice fn, IDXGIDevice *dev, IDCompositionDevice **out, DWORD *exc_out)
{
    *exc_out = 0;
    __try {
        return fn(dev, __uuidof(IDCompositionDevice), reinterpret_cast<void **>(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc_out = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

template <typename T> void release(T *&p)
{
    if (p) p->Release();
    p = nullptr;
}

struct Results {
    int device_ok = 0;
    int device_fail = 0;
};

// Exercises the operations the WSI-over-DXGI design needs from a device: a texture, a GPU clear,
// the DXGI adapter behind the device, NT-handle and KMT sharing, and a shared D3D11 fence.
void exercise_device(const char *tag, ID3D11Device *dev, ID3D11DeviceContext *ctx, Results &r)
{
    bool ok = true;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    say("  [%s] CreateTexture2D 64x64 BGRA8 RT|SRV: 0x%08lX\n", tag, hr);
    ok &= SUCCEEDED(hr);
    if (tex) {
        ID3D11RenderTargetView *rtv = nullptr;
        hr = dev->CreateRenderTargetView(tex, nullptr, &rtv);
        say("  [%s] CreateRenderTargetView: 0x%08lX\n", tag, hr);
        if (rtv) {
            const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            ctx->ClearRenderTargetView(rtv, color);
            ctx->Flush();
            release(rtv);
        }
    }
    hr = dev->GetDeviceRemovedReason();
    say("  [%s] GetDeviceRemovedReason after clear+flush: 0x%08lX\n", tag, hr);
    ok &= hr == S_OK;

    IDXGIDevice *dxgi_dev = nullptr;
    hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev));
    say("  [%s] QI IDXGIDevice: 0x%08lX implemented by %s\n", tag, hr, vtbl_owner(dxgi_dev).c_str());
    ok &= SUCCEEDED(hr);
    if (dxgi_dev) {
        IDXGIAdapter *adapter = nullptr;
        hr = dxgi_dev->GetAdapter(&adapter);
        DXGI_ADAPTER_DESC desc = {};
        if (adapter) adapter->GetDesc(&desc);
        say("  [%s] IDXGIDevice::GetAdapter: 0x%08lX adapter '%s' LUID %08lX:%08lX implemented by %s\n", tag, hr,
            narrow(desc.Description).c_str(), (unsigned long)desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
            vtbl_owner(adapter).c_str());
        ok &= SUCCEEDED(hr);
        if (adapter) {
            IDXGIFactory1 *parent = nullptr;
            hr = adapter->GetParent(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&parent));
            say("  [%s] adapter GetParent(IDXGIFactory1): 0x%08lX implemented by %s\n", tag, hr, vtbl_owner(parent).c_str());
            release(parent);
        }
        release(adapter);
    }

    // NT-handle shared texture: the transport for a Vulkan import (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT).
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *shared = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &shared);
    say("  [%s] CreateTexture2D SHARED_NTHANDLE: 0x%08lX\n", tag, hr);
    if (shared) {
        IDXGIResource1 *res1 = nullptr;
        HANDLE nt = nullptr;
        hr = shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res1));
        if (res1) hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt);
        say("  [%s] IDXGIResource1::CreateSharedHandle: 0x%08lX handle %s\n", tag, hr, nt ? "non-null" : "null");
        ID3D11Device1 *dev1 = nullptr;
        dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1));
        if (dev1 && nt) {
            ID3D11Texture2D *opened = nullptr;
            hr = dev1->OpenSharedResource1(nt, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&opened));
            say("  [%s] OpenSharedResource1(NT): 0x%08lX\n", tag, hr);
            release(opened);
        }
        if (nt) CloseHandle(nt);
        release(dev1);
        release(res1);
        release(shared);
    }
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = dev->CreateTexture2D(&td, nullptr, &shared);
    if (shared) {
        IDXGIResource *res = nullptr;
        HANDLE kmt = nullptr;
        shared->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&res));
        if (res) hr = res->GetSharedHandle(&kmt);
        say("  [%s] KMT shared texture GetSharedHandle: 0x%08lX handle %s\n", tag, hr, kmt ? "non-null" : "null");
        release(res);
        release(shared);
    } else {
        say("  [%s] CreateTexture2D SHARED (KMT): 0x%08lX\n", tag, hr);
    }

    // Shared fence: the synchronization object between the Vulkan queue and the D3D11 context.
    ID3D11Device5 *dev5 = nullptr;
    ID3D11DeviceContext4 *ctx4 = nullptr;
    dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5));
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx4));
    if (dev5 && ctx4) {
        ID3D11Fence *fence = nullptr;
        hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&fence));
        say("  [%s] ID3D11Device5::CreateFence(SHARED): 0x%08lX\n", tag, hr);
        if (fence) {
            HANDLE fh = nullptr;
            hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh);
            say("  [%s] ID3D11Fence::CreateSharedHandle: 0x%08lX\n", tag, hr);
            if (fh) {
                ID3D11Fence *opened = nullptr;
                hr = dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&opened));
                say("  [%s] ID3D11Device5::OpenSharedFence: 0x%08lX\n", tag, hr);
                release(opened);
                CloseHandle(fh);
            }
            hr = ctx4->Signal(fence, 1);
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HRESULT hw = fence->SetEventOnCompletion(1, ev);
            DWORD w = WaitForSingleObject(ev, 2000);
            say("  [%s] Signal(1): 0x%08lX, SetEventOnCompletion: 0x%08lX, wait: %s, completed value %llu\n", tag, hr, hw,
                w == WAIT_OBJECT_0 ? "signaled" : "timeout", (unsigned long long)fence->GetCompletedValue());
            CloseHandle(ev);
            release(fence);
        }
    } else {
        say("  [%s] ID3D11Device5/ID3D11DeviceContext4 not available\n", tag);
    }
    release(ctx4);
    release(dev5);
    release(tex);
    if (ok) ++r.device_ok; else ++r.device_fail;
}

bool make_actctx_manifest(const std::wstring &path, const std::wstring &target)
{
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
        "  <assemblyIdentity type=\"win32\" name=\"bc250.wsi.dxgiredirect\" version=\"1.0.0.0\" processorArchitecture=\"amd64\"/>\r\n"
        "  <file name=\"dxgi.dll\" loadFrom=\"" + narrow(target) + "\"/>\r\n"
        "</assembly>\r\n";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, xml.data(), (DWORD)xml.size(), &written, nullptr);
    CloseHandle(f);
    return ok && written == xml.size();
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        say("usage: shadowtest.exe control|shadow|shadow-rebind|shadow-actctx|shadow-search|sysfirst\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const bool shadow = mode.rfind(L"shadow", 0) == 0;
    say("mode %s\n", narrow(mode).c_str());
    say("exe %s\n", module_path(nullptr).c_str());
    {
        typedef LONG(WINAPI * PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
        auto rtl = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        RTL_OSVERSIONINFOW v = {sizeof(v)};
        if (rtl) rtl(&v);
        say("os %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }

    const std::wstring sys_dxgi_path = system_path(L"dxgi.dll");
    const std::wstring sys_d3d11_path = system_path(L"d3d11.dll");
    const std::wstring sys_dcomp_path = system_path(L"dcomp.dll");
    HMODULE app_dxgi = nullptr, sys_dxgi = nullptr, sys_d3d11 = nullptr;

    say("stage L: loads\n");
    if (mode == L"sysfirst") {
        sys_dxgi = LoadLibraryW(sys_dxgi_path.c_str());
        say("  LoadLibraryW(System32 dxgi full path) -> %s\n", module_path(sys_dxgi).c_str());
        HMODULE by_name = LoadLibraryW(L"dxgi.dll");
        say("  then LoadLibraryW(\"dxgi.dll\") by name -> %s (%s)\n", module_path(by_name).c_str(),
            by_name == sys_dxgi ? "same module as System32" : "different module");
        app_dxgi = by_name == sys_dxgi ? nullptr : by_name;
    } else {
        if (shadow) {
            app_dxgi = LoadLibraryW(L"dxgi.dll");
            say("  LoadLibraryW(\"dxgi.dll\") by name (game-style) -> %s\n", module_path(app_dxgi).c_str());
            std::wstring expect = exe_dir() + L"\\dxgi.dll";
            wchar_t got[1024];
            DWORD n = app_dxgi ? GetModuleFileNameW(app_dxgi, got, 1024) : 0;
            if (!app_dxgi || _wcsicmp(std::wstring(got, n).c_str(), expect.c_str())) {
                say("  ERROR: app-local dxgi.dll was not loaded from the executable directory\n");
                return 3;
            }
        }
        sys_dxgi = LoadLibraryW(sys_dxgi_path.c_str());
        say("  LoadLibraryW(System32 dxgi full path) -> %p %s (%s)\n", (void *)sys_dxgi, module_path(sys_dxgi).c_str(),
            sys_dxgi == app_dxgi ? "SAME module as app-local" : "separate module");
    }
    say("  GetModuleHandleW(\"dxgi.dll\") now -> %s\n", module_path(GetModuleHandleW(L"dxgi.dll")).c_str());

    HANDLE actctx = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie = 0;
    if (mode == L"shadow-actctx") {
        std::wstring manifest = exe_dir() + L"\\dxgiredirect.manifest";
        bool wrote = make_actctx_manifest(manifest, sys_dxgi_path);
        ACTCTXW ac = {sizeof(ac)};
        ac.lpSource = manifest.c_str();
        actctx = wrote ? CreateActCtxW(&ac) : INVALID_HANDLE_VALUE;
        say("  CreateActCtxW(<file name=dxgi.dll loadFrom=System32>): %s (error %lu)\n",
            actctx == INVALID_HANDLE_VALUE ? "FAILED" : "ok", actctx == INVALID_HANDLE_VALUE ? GetLastError() : 0ul);
        if (actctx != INVALID_HANDLE_VALUE) {
            BOOL a = ActivateActCtx(actctx, &cookie);
            say("  ActivateActCtx: %s\n", a ? "ok" : "FAILED");
            HMODULE probe = GetModuleHandleW(L"dxgi.dll");
            say("  inside actctx GetModuleHandleW(\"dxgi.dll\") -> %s\n", module_path(probe).c_str());
            HMODULE probe2 = LoadLibraryW(L"dxgi.dll");
            say("  inside actctx LoadLibraryW(\"dxgi.dll\") -> %s\n", module_path(probe2).c_str());
            if (probe2) FreeLibrary(probe2);
        }
    }

    if (mode == L"shadow-search") {
        sys_d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        say("  LoadLibraryExW(\"d3d11.dll\", LOAD_LIBRARY_SEARCH_SYSTEM32) -> %s\n", module_path(sys_d3d11).c_str());
    } else {
        sys_d3d11 = LoadLibraryW(sys_d3d11_path.c_str());
        say("  LoadLibraryW(System32 d3d11 full path) -> %s (error %lu)\n", module_path(sys_d3d11).c_str(),
            sys_d3d11 ? 0ul : GetLastError());
    }
    if (actctx != INVALID_HANDLE_VALUE) {
        if (cookie) DeactivateActCtx(0, cookie);
        ReleaseActCtx(actctx);
    }
    if (!sys_dxgi || !sys_d3d11) {
        say("RESULT load failure\n");
        return 4;
    }

    say("stage I: d3d11 import binding\n");
    report_static("d3d11", sys_d3d11, "dxgi.dll");
    if (mode == L"shadow-rebind") {
        int n = rebind_static(sys_d3d11, "dxgi.dll", sys_dxgi);
        say("  rebind d3d11 static IAT -> System32 dxgi: %d slot(s) rewritten\n", n);
        report_static("d3d11 after rebind", sys_d3d11, "dxgi.dll");
    }

    say("stage F: System32 DXGI factory and adapter\n");
    auto create_factory = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(sys_dxgi, "CreateDXGIFactory2"));
    IDXGIFactory1 *factory = nullptr;
    DWORD exc = 0;
    HRESULT hr = create_factory ? seh_create_factory(create_factory, &factory, &exc) : E_NOINTERFACE;
    say("  System32 CreateDXGIFactory2: 0x%08lX exc 0x%08lX implemented by %s\n", hr, exc, vtbl_owner(factory).c_str());
    IDXGIAdapter1 *adapter = nullptr;
    if (factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1 *a = nullptr;
            if (FAILED(factory->EnumAdapters1(i, &a))) break;
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            say("  adapter %u '%s' LUID %08lX:%08lX flags 0x%X\n", i, narrow(d.Description).c_str(),
                (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, d.Flags);
            if (!adapter && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) adapter = a;
            else a->Release();
        }
        say("  chosen adapter implemented by %s\n", vtbl_owner(adapter).c_str());
    }

    Results results;
    auto create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(sys_d3d11, "D3D11CreateDevice"));
    IDXGIDevice *keep_dxgi_device = nullptr;

    say("stage D1: System32 D3D11CreateDevice(explicit System32 adapter, UNKNOWN)\n");
    {
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        D3D_FEATURE_LEVEL fl = {};
        hr = adapter ? seh_create_device(create_device, adapter, D3D_DRIVER_TYPE_UNKNOWN, &dev, &fl, &ctx, &exc) : E_FAIL;
        say("  D3D11CreateDevice: 0x%08lX exc 0x%08lX FL 0x%X device implemented by %s\n", hr, exc, fl, vtbl_owner(dev).c_str());
        if (dev && ctx) {
            exercise_device("D1", dev, ctx, results);
            dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&keep_dxgi_device));
        } else {
            ++results.device_fail;
        }
        release(ctx);
        release(dev);
    }
    report_static("d3d11 after D1", sys_d3d11, "dxgi.dll");

    say("stage D2: System32 D3D11CreateDevice(NULL adapter, HARDWARE)\n");
    {
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        D3D_FEATURE_LEVEL fl = {};
        hr = seh_create_device(create_device, nullptr, D3D_DRIVER_TYPE_HARDWARE, &dev, &fl, &ctx, &exc);
        say("  D3D11CreateDevice: 0x%08lX exc 0x%08lX FL 0x%X device implemented by %s\n", hr, exc, fl, vtbl_owner(dev).c_str());
        if (dev && ctx) exercise_device("D2", dev, ctx, results);
        else ++results.device_fail;
        release(ctx);
        release(dev);
    }

    say("stage C: System32 DComp (Mesa's dcomp_get_device path)\n");
    HMODULE sys_dcomp = LoadLibraryW(sys_dcomp_path.c_str());
    say("  LoadLibraryW(System32 dcomp full path) -> %s\n", module_path(sys_dcomp).c_str());
    if (sys_dcomp) {
        report_delay("dcomp before use", sys_dcomp, "dxgi.dll");
        report_delay("dcomp before use", sys_dcomp, "d3d11.dll");
        if (mode == L"shadow-rebind") {
            int a = rebind_delay(sys_dcomp, "dxgi.dll", sys_dxgi);
            int b = rebind_delay(sys_dcomp, "d3d11.dll", sys_d3d11);
            say("  rebind dcomp delay IAT: dxgi %d, d3d11 %d slot(s) rewritten\n", a, b);
        }
        auto dcomp_create = reinterpret_cast<PFN_DCompositionCreateDevice>(GetProcAddress(sys_dcomp, "DCompositionCreateDevice"));
        IDCompositionDevice *dc = nullptr;
        hr = dcomp_create ? seh_dcomp(dcomp_create, nullptr, &dc, &exc) : E_NOINTERFACE;
        say("  DCompositionCreateDevice(NULL): 0x%08lX exc 0x%08lX\n", hr, exc);
        if (dc) {
            IDCompositionVisual *v = nullptr;
            HRESULT hv = dc->CreateVisual(&v);
            HRESULT hc = dc->Commit();
            say("  CreateVisual 0x%08lX, Commit 0x%08lX\n", hv, hc);
            release(v);
            release(dc);
        }
        if (keep_dxgi_device) {
            hr = dcomp_create ? seh_dcomp(dcomp_create, keep_dxgi_device, &dc, &exc) : E_NOINTERFACE;
            say("  DCompositionCreateDevice(D1 IDXGIDevice): 0x%08lX exc 0x%08lX\n", hr, exc);
            if (dc) {
                IDCompositionVisual *v = nullptr;
                HRESULT hv = dc->CreateVisual(&v);
                HRESULT hc = dc->Commit();
                say("  CreateVisual 0x%08lX, Commit 0x%08lX\n", hv, hc);
                release(v);
                release(dc);
            }
        }
        report_delay("dcomp after use", sys_dcomp, "dxgi.dll");
        report_delay("dcomp after use", sys_dcomp, "d3d11.dll");
    }
    report_delay("System32 dxgi", sys_dxgi, "dcomp.dll");

    say("stage M: final module state\n");
    say("  GetModuleHandleW(\"dxgi.dll\") -> %s\n", module_path(GetModuleHandleW(L"dxgi.dll")).c_str());
    say("  GetModuleHandleW(\"d3d11.dll\") -> %s\n", module_path(GetModuleHandleW(L"d3d11.dll")).c_str());
    list_modules("final");

    release(keep_dxgi_device);
    release(adapter);
    release(factory);
    say("RESULT mode=%s devices_ok=%d devices_failed=%d\n", narrow(mode).c_str(), results.device_ok, results.device_fail);
    return 0;
}
