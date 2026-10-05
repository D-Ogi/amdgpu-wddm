// SPDX-License-Identifier: MIT
// presenttest: headless probe of the in-process System32 D3D11 presenter route for the Vulkan WSI
// (M16 WSI-over-DXGI design), run inside simulated game processes.
//
// Scenario (argv[1] prefix):
//   control   no application-local graphics DLLs (pure Vulkan game)
//   dxvk11    application-local DXVK d3d11.dll + dxgi.dll, loaded by name first (DXVK D3D11 game)
//   vkd3d     application-local DXVK dxgi.dll + vkd3d-proton d3d12.dll, loaded by name first (D3D12 game)
// Policy (argv[1] suffix):
//   plain     the route as Mesa's DXGI WSI would run it: System32 modules by full path, explicit adapter
//   seal      plain + the proposed seal: d3d11 IAT and dcomp delay IAT rebound to System32 modules, and
//             bare-name d3d11/dxgi/dcomp loads issued by System32 graphics modules redirected to System32
//
// Route exercised (no window, no HWND target, no resident process):
//   System32 dxgi/dcomp/d3d11 by full path -> System32 factory -> adapter by LUID -> presenter D3D11 device
//   and producer D3D11 device (stand-in for the RADV device) -> NT-handle shared texture created by the
//   presenter, opened by the producer -> shared fence created by the producer, opened by the presenter ->
//   DComp device and visual (no target) -> CreateSwapChainForComposition FLIP_SEQUENTIAL -> 4 frames of
//   producer clear + Signal, presenter Wait + CopyResource into the back buffer + Present1 -> ResizeBuffers ->
//   ALLOW_TEARING swap chain if supported -> teardown -> late game-style loads by name.
// Every LoadLibrary*/GetModuleHandle* call made from a System32 graphics module is traced through its IAT.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

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

std::wstring widen(const char *s)
{
    if (!s) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

std::wstring lower(std::wstring s)
{
    for (auto &c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring g_system_dir;  // lower case, with trailing backslash
std::wstring g_exe_dir;     // lower case, with trailing backslash

std::wstring system_path(const wchar_t *name)
{
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir, n) + L"\\" + name;
}

std::wstring module_file(HMODULE m)
{
    if (!m) return {};
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(m, buf, 1024);
    return std::wstring(buf, n);
}

bool in_system32(HMODULE m)
{
    return m && lower(module_file(m)).rfind(g_system_dir, 0) == 0;
}

// Driver Store folder names are machine-specific; keep the class and file name only.
std::string describe(HMODULE m)
{
    if (!m) return "(null)";
    std::wstring p = module_file(m);
    std::wstring l = lower(p);
    std::wstring base = p.substr(p.find_last_of(L'\\') + 1);
    if (l.find(L"\\driverstore\\") != std::wstring::npos) return "<DriverStore>\\" + narrow(base);
    if (l.rfind(g_system_dir, 0) == 0) return "System32\\" + narrow(base);
    if (l.rfind(g_exe_dir, 0) == 0) return "APPDIR\\" + narrow(base);
    return narrow(p);
}

HMODULE module_of(const void *p)
{
    HMODULE m = nullptr;
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(p), &m))
        return nullptr;
    return m;
}

HMODULE vtbl_module(IUnknown *o)
{
    if (!o) return nullptr;
    void **vt = *reinterpret_cast<void ***>(o);
    return module_of(vt[3]);
}

std::wstring base_of(const std::wstring &path)
{
    size_t s = path.find_last_of(L"\\/");
    return lower(s == std::wstring::npos ? path : path.substr(s + 1));
}

bool graphics_base(std::wstring b)
{
    b = lower(b);
    if (b.size() > 4 && b.compare(b.size() - 4, 4, L".dll") == 0) b.resize(b.size() - 4);
    for (const wchar_t *p : {L"dxgi", L"d3d11", L"d3d12", L"d3d12core", L"dcomp", L"dxcore", L"d3d10warp", L"d3d11on12"})
        if (b == p) return true;
    return false;
}

// ---- PE import walking -----------------------------------------------------------------------------------------

IMAGE_NT_HEADERS64 *nt_of(HMODULE mod)
{
    auto *base = reinterpret_cast<BYTE *>(mod);
    return reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
}

struct ImportEntry {
    std::string dll;
    std::string name;  // empty when by ordinal
    WORD ordinal = 0;
    void **slot = nullptr;
    bool delay = false;
    HMODULE *delay_module_slot = nullptr;
};

std::vector<ImportEntry> imports_of(HMODULE mod)
{
    std::vector<ImportEntry> out;
    auto *base = reinterpret_cast<BYTE *>(mod);
    const auto &dirs = nt_of(mod)->OptionalHeader.DataDirectory;
    if (dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
        for (auto *d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress); d->Name; ++d) {
            const char *dll = reinterpret_cast<const char *>(base + d->Name);
            auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            auto *iat = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++iat) {
                ImportEntry e;
                e.dll = dll;
                e.slot = reinterpret_cast<void **>(&iat->u1.Function);
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) e.ordinal = (WORD)IMAGE_ORDINAL64(names->u1.Ordinal);
                else e.name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name;
                out.push_back(e);
            }
        }
    }
    if (dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress) {
        for (auto *d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR *>(base + dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress); d->DllNameRVA; ++d) {
            const char *dll = reinterpret_cast<const char *>(base + d->DllNameRVA);
            auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + d->ImportNameTableRVA);
            auto *iat = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + d->ImportAddressTableRVA);
            for (; names->u1.AddressOfData; ++names, ++iat) {
                ImportEntry e;
                e.dll = dll;
                e.delay = true;
                e.delay_module_slot = reinterpret_cast<HMODULE *>(base + d->ModuleHandleRVA);
                e.slot = reinterpret_cast<void **>(&iat->u1.Function);
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) e.ordinal = (WORD)IMAGE_ORDINAL64(names->u1.Ordinal);
                else e.name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name;
                out.push_back(e);
            }
        }
    }
    return out;
}

std::string entry_name(const ImportEntry &e)
{
    return e.name.empty() ? ("#" + std::to_string(e.ordinal)) : e.name;
}

bool write_slot(void **slot, void *value)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *slot = value;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old, &ignored);
    return true;
}

// ---- loader-call tracing and the optional bare-name redirect ---------------------------------------------------

SRWLOCK g_lock = SRWLOCK_INIT;
// Heap objects that are never destroyed: DXGI, DComp and driver worker threads may still call the hooks while
// the CRT runs static destructors at exit.
std::string &g_stage = *new std::string("S00 start");
std::map<std::string, int> &g_events = *new std::map<std::string, int>();
std::vector<std::string> &g_leaks = *new std::vector<std::string>();
bool g_redirect = false;
volatile LONG g_off = 0;  // set before main returns: hooks then forward without recording

void set_stage(const char *s)
{
    AcquireSRWLockExclusive(&g_lock);
    g_stage = s;
    ReleaseSRWLockExclusive(&g_lock);
    say("stage %s\n", s);
}

void add_leak(const std::string &what)
{
    AcquireSRWLockExclusive(&g_lock);
    g_leaks.push_back(g_stage + ": " + what);
    ReleaseSRWLockExclusive(&g_lock);
}

// A bare module name (no path) of a graphics module that a System32 graphics module asked for.
bool redirect_target(const std::wstring &requested, std::wstring &out)
{
    if (!g_redirect || requested.find_first_of(L"\\/") != std::wstring::npos) return false;
    std::wstring b = lower(requested);
    if (b.size() > 4 && b.compare(b.size() - 4, 4, L".dll") == 0) b.resize(b.size() - 4);
    // d3d12 is deliberately not redirected: a System32 d3d12.dll would bring System32 D3D12Core.dll into a
    // vkd3d-proton process, where it would shadow the application's d3d12core.dll for any later load by name.
    if (b == L"d3d11" || b == L"dxgi" || b == L"dcomp") {
        out = system_path((b + L".dll").c_str());
        return true;
    }
    return false;
}

void record(void *ret, const char *fn, const std::wstring &arg, HMODULE result, bool redirected)
{
    HMODULE caller = module_of(ret);
    std::string key = describe(caller) + " " + fn + "(" + narrow(arg) + ") -> " + describe(result) +
                      (redirected ? " [redirected]" : "");
    bool leaked = result && !in_system32(result) && graphics_base(base_of(module_file(result)));
    AcquireSRWLockExclusive(&g_lock);
    ++g_events[g_stage + " | " + key];
    if (leaked) g_leaks.push_back(g_stage + ": " + key);
    ReleaseSRWLockExclusive(&g_lock);
}

HMODULE WINAPI hk_LoadLibraryW(LPCWSTR name)
{
    if (g_off) return LoadLibraryW(name);
    void *ret = _ReturnAddress();
    std::wstring req = name ? name : L"(null)", target;
    bool red = name && redirect_target(req, target);
    HMODULE r = LoadLibraryW(red ? target.c_str() : name);
    DWORD e = GetLastError();
    record(ret, "LoadLibraryW", req, r, red);
    SetLastError(e);
    return r;
}

HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    if (g_off) return LoadLibraryExW(name, file, flags);
    void *ret = _ReturnAddress();
    std::wstring req = name ? name : L"(null)", target;
    const DWORD data = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    bool red = name && !(flags & data) && redirect_target(req, target);
    HMODULE r = LoadLibraryExW(red ? target.c_str() : name, file, flags);
    DWORD e = GetLastError();
    char fn[64];
    snprintf(fn, sizeof(fn), "LoadLibraryExW[0x%lX]", flags);
    record(ret, fn, req, r, red);
    SetLastError(e);
    return r;
}

HMODULE WINAPI hk_LoadLibraryA(LPCSTR name)
{
    if (g_off) return LoadLibraryA(name);
    void *ret = _ReturnAddress();
    std::wstring req = name ? widen(name) : L"(null)", target;
    bool red = name && redirect_target(req, target);
    HMODULE r = red ? LoadLibraryW(target.c_str()) : LoadLibraryA(name);
    DWORD e = GetLastError();
    record(ret, "LoadLibraryA", req, r, red);
    SetLastError(e);
    return r;
}

HMODULE WINAPI hk_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    if (g_off) return LoadLibraryExA(name, file, flags);
    void *ret = _ReturnAddress();
    std::wstring req = name ? widen(name) : L"(null)", target;
    const DWORD data = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    bool red = name && !(flags & data) && redirect_target(req, target);
    HMODULE r = red ? LoadLibraryExW(target.c_str(), file, flags) : LoadLibraryExA(name, file, flags);
    DWORD e = GetLastError();
    char fn[64];
    snprintf(fn, sizeof(fn), "LoadLibraryExA[0x%lX]", flags);
    record(ret, fn, req, r, red);
    SetLastError(e);
    return r;
}

HMODULE WINAPI hk_GetModuleHandleW(LPCWSTR name)
{
    if (g_off) return GetModuleHandleW(name);
    void *ret = _ReturnAddress();
    std::wstring target;
    bool red = name && redirect_target(name, target);
    HMODULE r = GetModuleHandleW(red ? target.c_str() : name);
    DWORD e = GetLastError();
    if (name) record(ret, "GetModuleHandleW", name, r, red);
    SetLastError(e);
    return r;
}

HMODULE WINAPI hk_GetModuleHandleA(LPCSTR name)
{
    if (g_off) return GetModuleHandleA(name);
    void *ret = _ReturnAddress();
    std::wstring req = name ? widen(name) : L"", target;
    bool red = name && redirect_target(req, target);
    HMODULE r = red ? GetModuleHandleW(target.c_str()) : GetModuleHandleA(name);
    DWORD e = GetLastError();
    if (name) record(ret, "GetModuleHandleA", req, r, red);
    SetLastError(e);
    return r;
}

BOOL WINAPI hk_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
{
    if (g_off) return GetModuleHandleExW(flags, name, out);
    void *ret = _ReturnAddress();
    BOOL ok = GetModuleHandleExW(flags, name, out);
    DWORD e = GetLastError();
    if (name && !(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
        record(ret, "GetModuleHandleExW", name, ok && out ? *out : nullptr, false);
    SetLastError(e);
    return ok;
}

BOOL WINAPI hk_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out)
{
    if (g_off) return GetModuleHandleExA(flags, name, out);
    void *ret = _ReturnAddress();
    BOOL ok = GetModuleHandleExA(flags, name, out);
    DWORD e = GetLastError();
    if (name && !(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
        record(ret, "GetModuleHandleExA", widen(name), ok && out ? *out : nullptr, false);
    SetLastError(e);
    return ok;
}

struct HookDef {
    const char *name;
    void *fn;
};
const HookDef g_hook_defs[] = {
    {"LoadLibraryW", reinterpret_cast<void *>(&hk_LoadLibraryW)},
    {"LoadLibraryExW", reinterpret_cast<void *>(&hk_LoadLibraryExW)},
    {"LoadLibraryA", reinterpret_cast<void *>(&hk_LoadLibraryA)},
    {"LoadLibraryExA", reinterpret_cast<void *>(&hk_LoadLibraryExA)},
    {"GetModuleHandleW", reinterpret_cast<void *>(&hk_GetModuleHandleW)},
    {"GetModuleHandleA", reinterpret_cast<void *>(&hk_GetModuleHandleA)},
    {"GetModuleHandleExW", reinterpret_cast<void *>(&hk_GetModuleHandleExW)},
    {"GetModuleHandleExA", reinterpret_cast<void *>(&hk_GetModuleHandleExA)},
};

std::vector<HMODULE> g_hooked;

// Installs the tracing hooks into every System32 graphics module currently loaded (idempotent).
void hook_system_graphics_modules()
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(GetCurrentProcess(), mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return;
    for (size_t i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        HMODULE m = mods[i];
        if (!in_system32(m) || !graphics_base(base_of(module_file(m)))) continue;
        bool seen = false;
        for (HMODULE h : g_hooked) seen |= h == m;
        if (seen) continue;
        int n = 0;
        for (const auto &e : imports_of(m)) {
            if (e.name.empty()) continue;
            for (const auto &h : g_hook_defs)
                if (e.name == h.name && *e.slot != h.fn && write_slot(e.slot, h.fn)) ++n;
        }
        g_hooked.push_back(m);
        say("  trace hooks installed in %s: %d IAT slot(s)\n", describe(m).c_str(), n);
    }
}

// ---- binding reports and the seal --------------------------------------------------------------------------------

void report_graphics_imports(const char *tag, HMODULE mod)
{
    for (const auto &e : imports_of(mod)) {
        std::wstring dll = widen(e.dll.c_str());
        if (!graphics_base(dll)) continue;
        std::string target;
        HMODULE owner = nullptr;
        if (e.delay) {
            HMODULE slotmod = e.delay_module_slot ? *e.delay_module_slot : nullptr;
            owner = slotmod;
            target = slotmod ? describe(slotmod) : "(unresolved)";
        } else {
            owner = module_of(*e.slot);
            target = describe(owner);
        }
        say("  [%s] %s %s!%s -> %s\n", tag, e.delay ? "delay" : "static", e.dll.c_str(), entry_name(e).c_str(), target.c_str());
        if (owner && !in_system32(owner)) add_leak(std::string(tag) + " " + e.dll + "!" + entry_name(e) + " bound to " + target);
    }
}

// Rebinds graphics imports of a System32 module to the already loaded System32 module of the same name.
// Never loads anything: an eager load of an unused delay-import target (d3d11on12 -> d3d12) would itself
// bind to application-local modules.
int seal_module(HMODULE mod)
{
    int changed = 0;
    for (const auto &e : imports_of(mod)) {
        std::wstring dll = widen(e.dll.c_str());
        if (!graphics_base(dll)) continue;
        std::wstring b = base_of(dll);
        if (b.size() <= 4 || b.compare(b.size() - 4, 4, L".dll") != 0) b += L".dll";
        HMODULE target = GetModuleHandleW(system_path(b.c_str()).c_str());
        if (!target || !in_system32(target)) continue;
        void *want = e.name.empty() ? reinterpret_cast<void *>(GetProcAddress(target, MAKEINTRESOURCEA(e.ordinal)))
                                    : reinterpret_cast<void *>(GetProcAddress(target, e.name.c_str()));
        if (!want) continue;
        if (*e.slot != want && write_slot(e.slot, want)) ++changed;
        if (e.delay && e.delay_module_slot && *e.delay_module_slot != target &&
            write_slot(reinterpret_cast<void **>(e.delay_module_slot), target))
            ++changed;
    }
    return changed;
}

void list_graphics_modules(const char *title)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(GetCurrentProcess(), mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return;
    size_t n = needed / sizeof(HMODULE);
    say("  [%s] %zu modules; graphics-related:\n", title, n);
    for (size_t i = 0; i < n && i < 1024; ++i) {
        std::wstring b = base_of(module_file(mods[i]));
        if (b.rfind(L"d3d", 0) == 0 || b.rfind(L"dxgi", 0) == 0 || b.rfind(L"dcomp", 0) == 0 ||
            b.rfind(L"vulkan", 0) == 0 || b.rfind(L"dxcore", 0) == 0 || b.rfind(L"nvldumd", 0) == 0 ||
            b.rfind(L"nvwgf2um", 0) == 0 || b.rfind(L"nvoglv", 0) == 0)
            say("    %s\n", describe(mods[i]).c_str());
    }
}

// ---- SEH wrappers: a crash inside a runtime is a result, not the end of the experiment ----------------------

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);
typedef HRESULT(WINAPI *PFN_DCompositionCreateDevice)(IDXGIDevice *, REFIID, void **);

HRESULT seh_factory(PFN_CreateDXGIFactory2 fn, IDXGIFactory4 **out, DWORD *exc)
{
    *exc = 0;
    __try {
        return fn(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_device(PFN_D3D11_CREATE_DEVICE fn, IDXGIAdapter *adapter, ID3D11Device **dev, ID3D11DeviceContext **ctx,
                   D3D_FEATURE_LEVEL *fl, DWORD *exc)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    *exc = 0;
    __try {
        return fn(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                  D3D11_SDK_VERSION, dev, fl, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_dcomp(PFN_DCompositionCreateDevice fn, IDCompositionDevice **out, DWORD *exc)
{
    *exc = 0;
    __try {
        return fn(nullptr, __uuidof(IDCompositionDevice), reinterpret_cast<void **>(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_swapchain(IDXGIFactory2 *f, IUnknown *dev, const DXGI_SWAP_CHAIN_DESC1 *d, IDXGISwapChain1 **out, DWORD *exc)
{
    *exc = 0;
    __try {
        return f->CreateSwapChainForComposition(dev, d, nullptr, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_set_content(IDCompositionVisual *v, IUnknown *content, IDCompositionDevice *dc, DWORD *exc)
{
    *exc = 0;
    __try {
        HRESULT hr = v->SetContent(content);
        if (FAILED(hr)) return hr;
        return dc->Commit();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_present(IDXGISwapChain1 *sc, UINT sync, UINT flags, DWORD *exc)
{
    DXGI_PRESENT_PARAMETERS p = {};
    *exc = 0;
    __try {
        return sc->Present1(sync, flags, &p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

HRESULT seh_resize(IDXGISwapChain1 *sc, UINT w, UINT h, DWORD *exc)
{
    *exc = 0;
    __try {
        return sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return E_UNEXPECTED;
    }
}

template <typename T> void release(T *&p)
{
    if (p) p->Release();
    p = nullptr;
}

double ms_since(const LARGE_INTEGER &t0)
{
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    return 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
}

HRESULT clear_buffer(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const float c[4])
{
    ID3D11RenderTargetView *rtv = nullptr;
    HRESULT hr = dev->CreateRenderTargetView(tex, nullptr, &rtv);
    if (rtv) {
        ctx->ClearRenderTargetView(rtv, c);
        rtv->Release();
    }
    return hr;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        say("usage: presenttest.exe {control|dxvk11|vkd3d}-{plain|seal}\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const size_t dash = mode.find(L'-');
    const std::wstring scenario = mode.substr(0, dash);
    const std::wstring policy = dash == std::wstring::npos ? L"plain" : mode.substr(dash + 1);
    const bool seal = policy == L"seal";
    {
        wchar_t dir[MAX_PATH];
        UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        g_system_dir = lower(std::wstring(dir, n)) + L"\\";
        wchar_t buf[1024];
        DWORD m = GetModuleFileNameW(nullptr, buf, 1024);
        std::wstring p(buf, m);
        g_exe_dir = lower(p.substr(0, p.find_last_of(L'\\'))) + L"\\";
    }
    say("mode %s (scenario %s, policy %s)\n", narrow(mode).c_str(), narrow(scenario).c_str(), narrow(policy).c_str());
    {
        typedef LONG(WINAPI * PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
        auto rtl = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        RTL_OSVERSIONINFOW v = {sizeof(v)};
        if (rtl) rtl(&v);
        say("os %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }

    // ---- the game: application-local modules loaded by name before any Vulkan code runs -----------------------
    set_stage("S01 game-loads");
    HMODULE app_d3d11 = nullptr, app_dxgi = nullptr, app_d3d12 = nullptr;
    if (scenario == L"dxvk11") {
        app_d3d11 = LoadLibraryW(L"d3d11.dll");
        app_dxgi = GetModuleHandleW(L"dxgi.dll");
        say("  game LoadLibraryW(\"d3d11.dll\") -> %s; its dxgi import pulled %s\n", describe(app_d3d11).c_str(), describe(app_dxgi).c_str());
        if (!app_d3d11 || in_system32(app_d3d11) || !app_dxgi || in_system32(app_dxgi)) {
            say("RESULT error: application-local DXVK modules not loaded\n");
            return 3;
        }
        report_graphics_imports("game d3d11", app_d3d11);
    } else if (scenario == L"vkd3d") {
        app_dxgi = LoadLibraryW(L"dxgi.dll");
        app_d3d12 = LoadLibraryW(L"d3d12.dll");
        say("  game LoadLibraryW(\"dxgi.dll\") -> %s, LoadLibraryW(\"d3d12.dll\") -> %s\n", describe(app_dxgi).c_str(), describe(app_d3d12).c_str());
        if (!app_dxgi || in_system32(app_dxgi) || !app_d3d12 || in_system32(app_d3d12)) {
            say("RESULT error: application-local dxgi/d3d12 not loaded\n");
            return 3;
        }
    } else if (scenario != L"control") {
        say("RESULT error: unknown scenario\n");
        return 2;
    }

    if (policy == L"none") {  // exit-behaviour control: the game's modules only, no WSI route
        say("RESULT mode=%s route=skipped\n", narrow(mode).c_str());
        say("main returns; process teardown follows\n");
        return 0;
    }

    // ---- WSI init: what util_load_system_library does, plus System32 d3d11 for the presenter -------------------
    set_stage("S02 wsi-init");
    HMODULE sys_dxgi = LoadLibraryW(system_path(L"dxgi.dll").c_str());
    HMODULE sys_dcomp = LoadLibraryW(system_path(L"dcomp.dll").c_str());
    HMODULE sys_d3d11 = LoadLibraryW(system_path(L"d3d11.dll").c_str());
    say("  System32 by full path: dxgi %s, dcomp %s, d3d11 %s\n", describe(sys_dxgi).c_str(), describe(sys_dcomp).c_str(), describe(sys_d3d11).c_str());
    if (!in_system32(sys_dxgi) || !in_system32(sys_dcomp) || !in_system32(sys_d3d11)) {
        say("RESULT error: System32 modules not loaded\n");
        return 4;
    }
    hook_system_graphics_modules();

    // The gate the WSI would evaluate before choosing the DXGI route.
    HMODULE by_name_dxgi = GetModuleHandleW(L"dxgi.dll");
    HMODULE by_name_d3d11 = GetModuleHandleW(L"d3d11.dll");
    HMODULE d3d11_factory_import = nullptr;
    for (const auto &e : imports_of(sys_d3d11))
        if (!e.delay && _stricmp(e.dll.c_str(), "dxgi.dll") == 0 && e.name == "CreateDXGIFactory2") d3d11_factory_import = module_of(*e.slot);
    const bool shadowed = !in_system32(by_name_dxgi) || !in_system32(d3d11_factory_import);
    say("  gate: GetModuleHandleW(dxgi.dll) -> %s, GetModuleHandleW(d3d11.dll) -> %s, d3d11!CreateDXGIFactory2 import -> %s: %s\n",
        describe(by_name_dxgi).c_str(), describe(by_name_d3d11).c_str(), describe(d3d11_factory_import).c_str(),
        shadowed ? "SHADOWED" : "clean");
    if (seal) {
        g_redirect = true;
        int a = seal_module(sys_d3d11), b = seal_module(sys_dcomp), c = seal_module(sys_dxgi);
        say("  seal: rebound d3d11 %d, dcomp %d, dxgi %d slot(s); bare-name redirect on for System32 graphics modules\n", a, b, c);
    }
    report_graphics_imports("System32 d3d11", sys_d3d11);
    report_graphics_imports("System32 dcomp", sys_dcomp);
    report_graphics_imports("System32 dxgi", sys_dxgi);

    // ---- factory and adapter by LUID (vk_dxgi_find_adapter) ---------------------------------------------------
    set_stage("S03 factory");
    DWORD exc = 0;
    IDXGIFactory4 *factory = nullptr;
    auto create_factory = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(sys_dxgi, "CreateDXGIFactory2"));
    HRESULT hr = seh_factory(create_factory, &factory, &exc);
    say("  System32 CreateDXGIFactory2(IDXGIFactory4): 0x%08lX exc 0x%08lX implemented by %s\n", hr, exc, describe(vtbl_module(factory)).c_str());
    if (!factory) {
        say("RESULT error: no factory\n");
        return 5;
    }
    IDXGIAdapter1 *adapter = nullptr;
    LUID luid = {};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *a = nullptr;
        if (FAILED(factory->EnumAdapters1(i, &a))) break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !luid.LowPart && !luid.HighPart) luid = d.AdapterLuid;
        a->Release();
    }
    hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&adapter));
    say("  EnumAdapterByLuid(first hardware adapter): 0x%08lX implemented by %s\n", hr, describe(vtbl_module(adapter)).c_str());
    hook_system_graphics_modules();

    // ---- presenter device and producer device --------------------------------------------------------------------
    set_stage("S04 devices");
    auto create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(sys_d3d11, "D3D11CreateDevice"));
    ID3D11Device *dev = nullptr, *prod = nullptr;
    ID3D11DeviceContext *ctx = nullptr, *pctx = nullptr;
    D3D_FEATURE_LEVEL fl = {}, pfl = {};
    hr = seh_device(create_device, adapter, &dev, &ctx, &fl, &exc);
    say("  presenter D3D11CreateDevice(adapter): 0x%08lX exc 0x%08lX FL 0x%X implemented by %s\n", hr, exc, fl, describe(vtbl_module(dev)).c_str());
    hr = seh_device(create_device, adapter, &prod, &pctx, &pfl, &exc);
    say("  producer  D3D11CreateDevice(adapter): 0x%08lX exc 0x%08lX FL 0x%X\n", hr, exc, pfl);
    if (!dev || !ctx || !prod || !pctx) {
        say("RESULT error: device creation failed\n");
        return 6;
    }
    hook_system_graphics_modules();
    ID3D11Device5 *dev5 = nullptr, *prod5 = nullptr;
    ID3D11DeviceContext4 *ctx4 = nullptr, *pctx4 = nullptr;
    dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5));
    prod->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&prod5));
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx4));
    pctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&pctx4));

    // ---- transport: presenter-owned NT shared texture, producer-owned shared fence ----------------------------
    set_stage("S05 sharing");
    const UINT W = 256, H = 256;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *shared = nullptr, *pshared = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &shared);
    HANDLE tex_handle = nullptr;
    IDXGIResource1 *res1 = nullptr;
    if (shared) shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res1));
    HRESULT hs = res1 ? res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &tex_handle) : E_NOINTERFACE;
    release(res1);
    ID3D11Device1 *prod1 = nullptr;
    prod->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&prod1));
    HRESULT ho = prod1 && tex_handle ? prod1->OpenSharedResource1(tex_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&pshared)) : E_FAIL;
    release(prod1);
    say("  shared texture: create 0x%08lX, CreateSharedHandle 0x%08lX, producer OpenSharedResource1 0x%08lX\n", hr, hs, ho);
    ID3D11Fence *pfence = nullptr, *fence = nullptr;
    HANDLE fence_handle = nullptr;
    hr = prod5 ? prod5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&pfence)) : E_NOINTERFACE;
    hs = pfence ? pfence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle) : E_FAIL;
    ho = dev5 && fence_handle ? dev5->OpenSharedFence(fence_handle, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&fence)) : E_FAIL;
    say("  shared fence: producer CreateFence 0x%08lX, CreateSharedHandle 0x%08lX, presenter OpenSharedFence 0x%08lX\n", hr, hs, ho);
    if (tex_handle) CloseHandle(tex_handle);
    if (fence_handle) CloseHandle(fence_handle);

    // ---- DComp device and visual, as dcomp_get_device does (no HWND target in a headless run) ----------------
    set_stage("S06 dcomp");
    auto dcomp_create = reinterpret_cast<PFN_DCompositionCreateDevice>(GetProcAddress(sys_dcomp, "DCompositionCreateDevice"));
    IDCompositionDevice *dc = nullptr;
    IDCompositionVisual *visual = nullptr;
    hr = seh_dcomp(dcomp_create, &dc, &exc);
    HRESULT hv = dc ? dc->CreateVisual(&visual) : E_FAIL;
    say("  DCompositionCreateDevice(NULL): 0x%08lX exc 0x%08lX, CreateVisual 0x%08lX\n", hr, exc, hv);
    hook_system_graphics_modules();

    // ---- composition swap chain on the presenter device ------------------------------------------------------
    set_stage("S07 swapchain");
    IDXGIFactory2 *factory2 = nullptr;
    factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory2));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    IDXGISwapChain1 *sc = nullptr;
    hr = factory2 ? seh_swapchain(factory2, dev, &sd, &sc, &exc) : E_NOINTERFACE;
    say("  CreateSwapChainForComposition(presenter, 256x256 BGRA8, FLIP_SEQUENTIAL, 2): 0x%08lX exc 0x%08lX implemented by %s\n",
        hr, exc, describe(vtbl_module(sc)).c_str());
    if (sc && !in_system32(vtbl_module(sc))) add_leak("swap chain implemented outside System32");
    IDXGISwapChain3 *sc3 = nullptr;
    if (sc) sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&sc3));
    if (sc && visual) {
        hr = seh_set_content(visual, sc, dc, &exc);
        say("  visual SetContent(swap chain) + Commit: 0x%08lX exc 0x%08lX\n", hr, exc);
    }
    hook_system_graphics_modules();

    // ---- frames: producer renders and signals, presenter waits, copies and presents ---------------------------
    set_stage("S08 frames");
    int frames_ok = 0;
    bool pixel_ok = false;
    ID3D11RenderTargetView *prtv = nullptr;
    if (pshared) prod->CreateRenderTargetView(pshared, nullptr, &prtv);
    D3D11_TEXTURE2D_DESC stg = td;
    stg.BindFlags = 0; stg.MiscFlags = 0; stg.Usage = D3D11_USAGE_STAGING; stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    dev->CreateTexture2D(&stg, nullptr, &staging);
    for (UINT i = 0; sc3 && prtv && fence && pfence && staging && ctx4 && pctx4 && i < 4; ++i) {
        const float colors[4][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
        pctx->ClearRenderTargetView(prtv, colors[i]);
        HRESULT hsig = pctx4->Signal(pfence, i + 1);
        pctx->Flush();
        HRESULT hwait = ctx4->Wait(fence, i + 1);
        UINT idx = sc3->GetCurrentBackBufferIndex();
        ID3D11Texture2D *back = nullptr;
        HRESULT hb = sc3->GetBuffer(idx, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back));
        if (back) ctx->CopyResource(back, shared);
        if (back && i == 0) {
            ctx->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                const BYTE *p = static_cast<const BYTE *>(map.pData) + map.RowPitch * 17 + 4 * 33;
                pixel_ok = std::abs(p[0] - 191) <= 1 && std::abs(p[1] - 128) <= 1 && std::abs(p[2] - 64) <= 1 && p[3] == 255;
                say("  frame 0 back buffer pixel BGRA %u,%u,%u,%u (expected 191,128,64,255): %s\n", p[0], p[1], p[2], p[3],
                    pixel_ok ? "match" : "MISMATCH");
                ctx->Unmap(staging, 0);
            }
        }
        release(back);
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        HRESULT hp = seh_present(sc3, 1, 0, &exc);
        double ms = ms_since(t0);
        say("  frame %u: Signal 0x%08lX Wait 0x%08lX GetBuffer(%u) 0x%08lX Present1(1,0) 0x%08lX exc 0x%08lX %.3f ms\n", i, hsig, hwait,
            idx, hb, hp, exc, ms);
        if (SUCCEEDED(hsig) && SUCCEEDED(hwait) && SUCCEEDED(hb) && SUCCEEDED(hp)) ++frames_ok;
    }
    say("  fence completed value %llu\n", fence ? (unsigned long long)fence->GetCompletedValue() : 0ull);
    release(staging);
    release(prtv);
    hook_system_graphics_modules();

    // ---- resize --------------------------------------------------------------------------------------------------
    set_stage("S09 resize");
    bool resize_ok = false;
    if (sc3) {
        ctx->ClearState();
        ctx->Flush();
        hr = seh_resize(sc3, 320, 200, &exc);
        ID3D11Texture2D *back = nullptr;
        HRESULT hb = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back));
        D3D11_TEXTURE2D_DESC bd = {};
        if (back) back->GetDesc(&bd);
        const float c[4] = {0.1f, 0.2f, 0.3f, 1};
        HRESULT hc = back ? clear_buffer(dev, ctx, back, c) : E_FAIL;
        release(back);
        HRESULT hp = seh_present(sc3, 0, 0, &exc);
        say("  ResizeBuffers(320x200) 0x%08lX, GetBuffer 0x%08lX (%ux%u), clear 0x%08lX, Present1(0,0) 0x%08lX exc 0x%08lX\n", hr, hb,
            bd.Width, bd.Height, hc, hp, exc);
        resize_ok = SUCCEEDED(hr) && SUCCEEDED(hb) && bd.Width == 320 && bd.Height == 200 && SUCCEEDED(hp);
    }

    // ---- IMMEDIATE present mode: ALLOW_TEARING --------------------------------------------------------------
    set_stage("S10 tearing");
    IDXGIFactory5 *factory5 = nullptr;
    BOOL allow = FALSE;
    factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&factory5));
    hr = factory5 ? factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)) : E_NOINTERFACE;
    say("  CheckFeatureSupport(PRESENT_ALLOW_TEARING): 0x%08lX allow %d\n", hr, allow);
    bool tearing_ok = !allow;
    if (allow && factory2 && visual) {
        DXGI_SWAP_CHAIN_DESC1 sd2 = sd;
        sd2.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        IDXGISwapChain1 *sc2 = nullptr;
        HRESULT hc = seh_swapchain(factory2, dev, &sd2, &sc2, &exc);
        HRESULT hsc = sc2 ? seh_set_content(visual, sc2, dc, &exc) : E_FAIL;
        ID3D11Texture2D *back = nullptr;
        if (sc2) sc2->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back));
        const float c[4] = {1, 1, 0, 1};
        if (back) clear_buffer(dev, ctx, back, c);
        release(back);
        HRESULT hp = sc2 ? seh_present(sc2, 0, DXGI_PRESENT_ALLOW_TEARING, &exc) : E_FAIL;
        say("  ALLOW_TEARING swap chain 0x%08lX, SetContent+Commit 0x%08lX, Present1(0, ALLOW_TEARING) 0x%08lX exc 0x%08lX\n", hc, hsc, hp, exc);
        tearing_ok = SUCCEEDED(hc) && SUCCEEDED(hsc) && SUCCEEDED(hp);
        if (visual) seh_set_content(visual, sc3, dc, &exc);
        release(sc2);
    }
    release(factory5);

    // ---- teardown ------------------------------------------------------------------------------------------------
    set_stage("S11 teardown");
    if (visual && dc) {
        hr = seh_set_content(visual, nullptr, dc, &exc);
        say("  visual SetContent(NULL) + Commit: 0x%08lX exc 0x%08lX\n", hr, exc);
    }
    release(sc3);
    release(sc);
    release(visual);
    release(dc);
    release(factory2);
    release(fence);
    release(pfence);
    release(pshared);
    release(shared);
    release(ctx4);
    release(pctx4);
    release(dev5);
    release(prod5);
    if (ctx) ctx->ClearState();
    release(pctx);
    release(prod);
    release(ctx);
    release(dev);
    release(adapter);
    release(factory);
    hook_system_graphics_modules();
    report_graphics_imports("System32 d3d11 final", sys_d3d11);
    report_graphics_imports("System32 dcomp final", sys_dcomp);
    report_graphics_imports("System32 dxgi final", sys_dxgi);
    for (HMODULE m : g_hooked)
        if (m != sys_d3d11 && m != sys_dcomp && m != sys_dxgi) report_graphics_imports(describe(m).c_str(), m);
    if (app_d3d11) report_graphics_imports("game d3d11 final", app_d3d11);

    // ---- the game after the WSI: later loads by name ----------------------------------------------------------
    set_stage("S12 late-game-loads");
    HMODULE late_d3d11 = LoadLibraryW(L"d3d11.dll");
    HMODULE late_dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE late_d3d12 = scenario == L"vkd3d" ? LoadLibraryW(L"d3d12.dll") : nullptr;
    say("  game LoadLibraryW by name after the WSI: d3d11 -> %s, dxgi -> %s%s%s\n", describe(late_d3d11).c_str(), describe(late_dxgi).c_str(),
        late_d3d12 ? ", d3d12 -> " : "", late_d3d12 ? describe(late_d3d12).c_str() : "");
    bool game_view_ok = true;
    if (app_d3d11) game_view_ok &= late_d3d11 == app_d3d11;
    if (app_dxgi) game_view_ok &= late_dxgi == app_dxgi;
    if (app_d3d12) game_view_ok &= late_d3d12 == app_d3d12;
    if (!app_d3d11) game_view_ok &= in_system32(late_d3d11);  // no app-local copy: System32 is also what search order gives
    if (!app_dxgi) game_view_ok &= in_system32(late_dxgi);
    say("  game view of its own modules unchanged: %s\n", game_view_ok ? "yes" : "NO");
    if (late_d3d11) FreeLibrary(late_d3d11);
    if (late_dxgi) FreeLibrary(late_dxgi);
    if (late_d3d12) FreeLibrary(late_d3d12);
    list_graphics_modules("final");

    // ---- trace and verdict ----------------------------------------------------------------------------------
    AcquireSRWLockExclusive(&g_lock);
    say("trace: %zu distinct loader calls from System32 graphics modules\n", g_events.size());
    for (const auto &kv : g_events) say("  %4d x %s\n", kv.second, kv.first.c_str());
    say("leaks: %zu\n", g_leaks.size());
    for (const auto &l : g_leaks) say("  LEAK %s\n", l.c_str());
    const size_t leaks = g_leaks.size();
    ReleaseSRWLockExclusive(&g_lock);
    const bool route_ok = frames_ok == 4 && pixel_ok && resize_ok && tearing_ok;
    InterlockedExchange(&g_off, 1);
    say("RESULT mode=%s gate=%s route=%s frames=%d pixel=%s resize=%s tearing=%s leaks=%zu game_view=%s\n", narrow(mode).c_str(),
        shadowed ? "shadowed" : "clean", route_ok ? "PASS" : "FAIL", frames_ok, pixel_ok ? "ok" : "bad", resize_ok ? "ok" : "bad",
        allow ? (tearing_ok ? "ok" : "bad") : "unsupported", leaks, game_view_ok ? "ok" : "changed");
    say("main returns; process teardown follows\n");
    return 0;
}
