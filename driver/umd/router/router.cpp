// SPDX-License-Identifier: MIT
//
// bc250d3d_router.dll - D3D10/D3D11 UMD router for UserModeDriverName entries 1 and 2 (D3D10/D3D11 runtimes).
//
// Desktop (unchanged from router 5BBEB783): dwm.exe (and any HostedClients entry) goes to the hosted GPU UMD
// bc250d3d_zink.dll, or to the CPU UMD by the kill switch or the KMD switches. Applications (every other process) go
// to the application GPU UMD (the DXVK-based amdgpu_wddm_d3d11.dll) or to the CPU UMD by the AppRouter policy. The
// route is decided at every OpenAdapter10/OpenAdapter10_2 call from:
//
//   HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter   (the GPU DWM kit's fixed interface; read for every process)
//     CpuUmdPath          REG_SZ     CPU UMD, absolute path (default: the compiled DefaultCpuUmdPath below)
//     HostedUmdPath       REG_SZ     hosted UMD, absolute path (default: bc250d3d_zink.dll next to this DLL)
//     DwmForceCpu         REG_DWORD  kill switch: any value other than 0 (or any other type) sends every hosted
//                                    client to the CPU UMD; absent = 0
//     RequireKmdSwitches  REG_DWORD  0 = ignore the KMD desktop switches; absent, 1 or any other value = require
//                                    both of them
//     HostedClients       REG_MULTI_SZ  further image base names routed like dwm.exe (test clients)
//     RouteLogDirectory   REG_SZ     when set: one line per OpenAdapter call to route-<exe>-<pid>.log there
//   HKLM\SOFTWARE\amdgpu-wddm\AppRouter       (applications only; never read for dwm.exe or a HostedClients entry)
//     Mode                REG_SZ     cpu | allowlist | gpu-default. Absent = cpu (the application kill switch); any
//                                    other string or value kind = invalid, which routes like cpu
//     GpuUmdPath          REG_SZ     application GPU UMD, absolute path; absent or not absolute = every application
//                                    on the CPU UMD (reason app-gpu-umd-unset). No compiled default.
//     Allow               REG_MULTI_SZ  image base names routed to the GPU UMD in allowlist mode
//     Deny                REG_MULTI_SZ  image base names kept on the CPU UMD in both modes (Deny wins over Allow).
//                                    A Deny or Allow value that cannot be read (other kind, too long) makes the mode
//                                    invalid (fail safe: CPU).
//     RouteLogDirectory   REG_SZ     application route lines go there; absent = DesktopRouter's RouteLogDirectory
//   Built in: logonui.exe, consent.exe, lockapp.exe, credentialuibroker.exe and winlogon.exe always stay on the CPU
//   UMD (router-policy.h IsProtectedApp). OpenAdapter10 (the D3D10.0 runtime) always goes to the CPU UMD: the
//   application GPU UMD exports OpenAdapter10_2 only.
//   HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters
//     InteropLastState    REG_DWORD, KMD 0.7.181 and later (driver/kmd/interop.c InteropStart): effective bits |
//                         requested bits << 8, written at every full adapter start. Both switches count as on
//                         when the effective bits 1 (EnableGpuPresentBlit) and 2 (EnableCddDwmInterop) are both
//                         set: the start-latched state, after the KMD's own closes (invalid setting, unclean boot,
//                         registry failure). Present but not a REG_DWORD: off.
//     EnableGpuPresentBlit, EnableCddDwmInterop   REG_DWORD, read only when InteropLastState is absent (KMD 0.7.180
//                         and earlier, whose wddm.c latched "value == 1" at adapter start): each on only when 1.
//
// Process-name test: the base name of GetModuleFileNameW(NULL) compared case-insensitively with "dwm.exe" (and
// with each HostedClients, Allow and Deny entry). A failed GPU load or a failed GPU OpenAdapter (hosted UMD for the
// desktop, application GPU UMD for applications) falls back to the CPU UMD with the caller's arguments restored, so
// a broken GPU path leaves the process on the CPU UMD; the route line says fallback=1 with the GPU HRESULT. A failure
// after OpenAdapter (CreateDevice, the engine or ICD load inside the GPU UMD) cannot be re-routed: the runtime has
// already negotiated the adapter's caps with the GPU UMD. Modules are loaded once per path and never unloaded
// (adapters opened through them may outlive any call here). Every decision is also sent to OutputDebugString (one
// line per OpenAdapter call). The desktop line is byte-compatible with router 5BBEB783 (the kit parses it).

#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)
#include <cstdio>
#include <cwchar>
#include "router-policy.h"

#pragma comment(lib, "advapi32.lib")

using namespace bc250router;

static const wchar_t DefaultCpuUmdPath[] = L"C:\\BC250\\m15\\desktop-umd173-007\\bc250d3d.dll";
static const wchar_t RouterKey[] = L"SOFTWARE\\amdgpu-wddm\\DesktopRouter";
static const wchar_t AppRouterKey[] = L"SOFTWARE\\amdgpu-wddm\\AppRouter";
static const wchar_t KmdParametersKey[] = L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters";
static const wchar_t HostedUmdFile[] = L"bc250d3d_zink.dll";
static const size_t PathChars = 1024;
static const DWORD ClientsBytes = 4096;
static const DWORD AppListBytes = 16384;

struct Config {
    wchar_t cpu[PathChars];
    const char *cpu_source;
    wchar_t hosted[PathChars];
    const char *hosted_source;
    wchar_t clients[ClientsBytes / sizeof(wchar_t)];
    wchar_t log_directory[PathChars];
    DWORD force_cpu, require_switches;
    bool blit_on, interop_on;
    const char *switch_source; // "latched" (InteropLastState), "settings" (older KMD), "latched-invalid", "unreadable"
};

// The application policy (AppRouter key), read only for processes the desktop decision does not route.
struct AppConfig {
    AppMode mode;
    const char *mode_source;     // "absent", "registry", "registry-invalid", "list-invalid", "key-absent"
    wchar_t gpu[PathChars];
    const char *gpu_source;      // "registry", "registry-invalid", "absent"
    wchar_t allow[AppListBytes / sizeof(wchar_t)];
    wchar_t deny[AppListBytes / sizeof(wchar_t)];
    wchar_t log_directory[PathChars];
};

static bool IsAbsolute(const wchar_t *p)
{
    return ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\';
}

// A value of the wanted type that fits; false for absent, other type or too large.
static bool ReadString(HKEY key, const wchar_t *name, wchar_t *out, size_t chars)
{
    DWORD bytes = (DWORD)(chars * sizeof(wchar_t));
    out[0] = 0;
    LSTATUS s = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, out, &bytes);
    if (s != ERROR_SUCCESS) out[0] = 0;
    return s == ERROR_SUCCESS && out[0];
}

enum class Dword { Absent, Value, Other };
static Dword ReadDword(HKEY key, const wchar_t *name, DWORD *value)
{
    DWORD bytes = sizeof(*value), type = 0;
    *value = 0;
    LSTATUS s = RegQueryValueExW(key, name, nullptr, &type, (BYTE *)value, &bytes);
    if (s == ERROR_FILE_NOT_FOUND) return Dword::Absent;
    if (s == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(*value)) return Dword::Value;
    *value = 0;
    return Dword::Other;
}

// Absent, a value of the kind that fits (stored in out, NUL-terminated; a multi-string also double-NUL-terminated),
// or anything else (other kind, too long, unreadable). out is empty unless Value.
enum class Text { Absent, Value, Other };
static Text ReadText(HKEY key, const wchar_t *name, DWORD flags, wchar_t *out, DWORD bytes)
{
    const DWORD chars = bytes / sizeof(wchar_t);
    ZeroMemory(out, bytes);
    DWORD size = bytes - 2 * sizeof(wchar_t); // keep two terminating NULs whatever the stored data holds
    LSTATUS s = RegGetValueW(key, nullptr, name, flags, nullptr, out, &size);
    out[chars - 1] = out[chars - 2] = 0;
    if (s == ERROR_FILE_NOT_FOUND) return Text::Absent;
    if (s == ERROR_SUCCESS) return Text::Value;
    ZeroMemory(out, bytes);
    return Text::Other;
}

static void ModuleDirectory(wchar_t *out, size_t chars)
{
    HMODULE self = nullptr;
    out[0] = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&ModuleDirectory, &self)) return;
    DWORD n = GetModuleFileNameW(self, out, (DWORD)chars);
    wchar_t *slash = n && n < chars ? wcsrchr(out, L'\\') : nullptr;
    if (slash) *slash = 0; else out[0] = 0;
}

static void ReadConfig(Config *c)
{
    ZeroMemory(c, sizeof(*c));
    c->require_switches = 1;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RouterKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (ReadString(key, L"CpuUmdPath", c->cpu, PathChars) && IsAbsolute(c->cpu)) c->cpu_source = "registry";
        else c->cpu[0] = 0;
        if (ReadString(key, L"HostedUmdPath", c->hosted, PathChars)) {
            // A present but unusable value must not silently pick the default: the hosted load then fails and
            // the route falls back to the CPU UMD, visible in the route line.
            c->hosted_source = IsAbsolute(c->hosted) ? "registry" : "registry-invalid";
        }
        DWORD v = 0;
        // Fail safe: a kill switch of the wrong type counts as set; a requirement of the wrong type as required.
        switch (ReadDword(key, L"DwmForceCpu", &v)) {
        case Dword::Absent: c->force_cpu = 0; break;
        case Dword::Value: c->force_cpu = v; break;
        case Dword::Other: c->force_cpu = 1; break;
        }
        c->require_switches = ReadDword(key, L"RequireKmdSwitches", &v) == Dword::Value && v == 0 ? 0 : 1;
        DWORD bytes = ClientsBytes;
        if (RegGetValueW(key, nullptr, L"HostedClients", RRF_RT_REG_MULTI_SZ, nullptr, c->clients, &bytes) != ERROR_SUCCESS)
            c->clients[0] = c->clients[1] = 0;
        c->clients[ARRAYSIZE(c->clients) - 1] = 0;
        c->clients[ARRAYSIZE(c->clients) - 2] = 0;
        if (!ReadString(key, L"RouteLogDirectory", c->log_directory, PathChars) || !IsAbsolute(c->log_directory))
            c->log_directory[0] = 0;
        RegCloseKey(key);
    }
    if (!c->cpu[0]) {
        wcscpy_s(c->cpu, DefaultCpuUmdPath);
        c->cpu_source = "default";
    }
    if (!c->hosted_source) {
        wchar_t dir[PathChars];
        ModuleDirectory(dir, PathChars);
        if (dir[0] && _snwprintf_s(c->hosted, PathChars, _TRUNCATE, L"%ls\\%ls", dir, HostedUmdFile) > 0)
            c->hosted_source = "router-directory";
        else {
            c->hosted[0] = 0;
            c->hosted_source = "unavailable";
        }
    }
    c->switch_source = "unreadable";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KmdParametersKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD v = 0;
        switch (ReadDword(key, L"InteropLastState", &v)) {
        case Dword::Value:
            c->switch_source = "latched";
            c->blit_on = (v & 1) != 0;
            c->interop_on = (v & 2) != 0;
            break;
        case Dword::Other:
            c->switch_source = "latched-invalid";
            break;
        case Dword::Absent:
            c->switch_source = "settings";
            c->blit_on = ReadDword(key, L"EnableGpuPresentBlit", &v) == Dword::Value && v == 1;
            c->interop_on = ReadDword(key, L"EnableCddDwmInterop", &v) == Dword::Value && v == 1;
            break;
        }
        RegCloseKey(key);
    }
}

// The AppRouter key. Fail safe throughout: anything unreadable keeps applications on the CPU UMD.
static void ReadAppConfig(AppConfig *a)
{
    ZeroMemory(a, sizeof(*a));
    a->mode = AppMode::Cpu;
    a->mode_source = "key-absent";
    a->gpu_source = "absent";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AppRouterKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    wchar_t text[64];
    switch (ReadText(key, L"Mode", RRF_RT_REG_SZ, text, sizeof(text))) {
    case Text::Absent: a->mode = AppMode::Cpu; a->mode_source = "absent"; break;
    case Text::Value:
        a->mode = ParseAppMode(text);
        a->mode_source = a->mode == AppMode::Invalid ? "registry-invalid" : "registry";
        break;
    case Text::Other: a->mode = AppMode::Invalid; a->mode_source = "registry-invalid"; break;
    }
    switch (ReadText(key, L"GpuUmdPath", RRF_RT_REG_SZ, a->gpu, sizeof(a->gpu))) {
    case Text::Absent: a->gpu_source = "absent"; break;
    case Text::Value:
        a->gpu_source = a->gpu[0] && IsAbsolute(a->gpu) ? "registry" : "registry-invalid";
        break;
    case Text::Other: a->gpu_source = "registry-invalid"; break;
    }
    if (strcmp(a->gpu_source, "registry")) a->gpu[0] = 0;
    // A list that exists but cannot be read must not widen the GPU route: the whole mode turns invalid.
    const Text deny = ReadText(key, L"Deny", RRF_RT_REG_MULTI_SZ, a->deny, sizeof(a->deny));
    const Text allow = ReadText(key, L"Allow", RRF_RT_REG_MULTI_SZ, a->allow, sizeof(a->allow));
    if ((deny == Text::Other || allow == Text::Other) && a->mode != AppMode::Cpu) {
        a->mode = AppMode::Invalid;
        a->mode_source = "list-invalid";
    }
    if (ReadText(key, L"RouteLogDirectory", RRF_RT_REG_SZ, a->log_directory, sizeof(a->log_directory)) != Text::Value ||
        !IsAbsolute(a->log_directory))
        a->log_directory[0] = 0;
    RegCloseKey(key);
}

static const wchar_t *ExeBase(wchar_t *buffer, size_t chars)
{
    DWORD n = GetModuleFileNameW(nullptr, buffer, (DWORD)chars);
    if (!n || n >= chars) return L"";
    const wchar_t *slash = wcsrchr(buffer, L'\\');
    return slash ? slash + 1 : buffer;
}

// Loaded modules by path; never freed.
static SRWLOCK ModulesLock = SRWLOCK_INIT;
static struct { wchar_t path[PathChars]; HMODULE module; } Modules[8];

static HMODULE Load(const wchar_t *path, DWORD *error)
{
    *error = 0;
    if (!path[0] || !IsAbsolute(path)) { *error = ERROR_BAD_PATHNAME; return nullptr; }
    AcquireSRWLockExclusive(&ModulesLock);
    HMODULE found = nullptr;
    size_t freeSlot = ARRAYSIZE(Modules);
    for (size_t i = 0; i < ARRAYSIZE(Modules); ++i) {
        if (Modules[i].module && !_wcsicmp(Modules[i].path, path)) { found = Modules[i].module; break; }
        if (!Modules[i].module && freeSlot == ARRAYSIZE(Modules)) freeSlot = i;
    }
    if (!found) {
        // Altered search path: the UMD's own dependencies resolve from its directory first.
        found = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!found) *error = GetLastError();
        else if (freeSlot < ARRAYSIZE(Modules)) { wcscpy_s(Modules[freeSlot].path, path); Modules[freeSlot].module = found; }
    }
    ReleaseSRWLockExclusive(&ModulesLock);
    return found;
}

static void WriteRouteLine(const wchar_t *directory, const wchar_t *exe, const char *line, int n)
{
    OutputDebugStringA(line);
    if (!directory[0]) return;
    wchar_t path[PathChars];
    if (_snwprintf_s(path, PathChars, _TRUNCATE, L"%ls\\route-%ls-%lu.log", directory, exe[0] ? exe : L"unknown",
                     GetCurrentProcessId()) < 0) return;
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, line, (DWORD)n, &written, nullptr);
    CloseHandle(f);
}

// The desktop line, unchanged from router 5BBEB783.
static void Report(const Config &c, const wchar_t *exe, const char *entry, const Decision &d, HRESULT hostedHr,
                   bool fellBack, const wchar_t *module, HRESULT hr)
{
    char line[2048];
    int n = _snprintf_s(line, _TRUNCATE,
        "bc250d3d_router pid=%lu exe=%ls entry=%s route=%s reason=%s fallback=%u hosted_hr=%08lx hr=%08lx module=%ls "
        "cpu_source=%s hosted_source=%s force_cpu=%lu require_switches=%lu switch_source=%s blit=%u interop=%u\n",
        GetCurrentProcessId(), exe, entry, d.route == Route::Hosted && !fellBack ? "hosted" : "cpu",
        ReasonName(d.reason), fellBack ? 1u : 0u, (unsigned long)hostedHr, (unsigned long)hr, module,
        c.cpu_source, c.hosted_source, c.force_cpu, c.require_switches, c.switch_source, c.blit_on ? 1u : 0u,
        c.interop_on ? 1u : 0u);
    if (n < 0) n = (int)strlen(line);
    WriteRouteLine(c.log_directory, exe, line, n);
}

// The application line: the same leading fields, gpu_hr in place of hosted_hr, then the AppRouter state.
static void ReportApp(const Config &c, const AppConfig &a, const wchar_t *exe, const char *entry, const AppDecision &d,
                      HRESULT gpuHr, bool fellBack, const wchar_t *module, HRESULT hr)
{
    char line[2048];
    int n = _snprintf_s(line, _TRUNCATE,
        "bc250d3d_router pid=%lu exe=%ls entry=%s route=%s reason=%s fallback=%u gpu_hr=%08lx hr=%08lx module=%ls "
        "app_mode=%s mode_source=%s gpu_source=%s cpu_source=%s\n",
        GetCurrentProcessId(), exe, entry, d.route == AppRoute::Gpu && !fellBack ? "gpu" : "cpu",
        AppReasonName(d.reason), fellBack ? 1u : 0u, (unsigned long)gpuHr, (unsigned long)hr, module,
        AppModeName(a.mode), a.mode_source, a.gpu_source, c.cpu_source);
    if (n < 0) n = (int)strlen(line);
    WriteRouteLine(a.log_directory[0] ? a.log_directory : c.log_directory, exe, line, n);
}

using OpenAdapterFn = HRESULT(APIENTRY *)(D3D10DDIARG_OPENADAPTER *);

static HRESULT CallEntry(HMODULE module, const char *entry, D3D10DDIARG_OPENADAPTER *args)
{
    auto fn = (OpenAdapterFn)GetProcAddress(module, entry);
    return fn ? fn(args) : E_NOINTERFACE;
}

// One GPU attempt: the caller's arguments and function table are restored when it fails, so that the CPU UMD sees
// exactly what the runtime passed.
static HRESULT TryGpu(const wchar_t *path, const char *entry, D3D10DDIARG_OPENADAPTER *args, size_t tableBytes)
{
    const D3D10DDIARG_OPENADAPTER savedArgs = *args;
    BYTE savedTable[sizeof(D3D10_2DDI_ADAPTERFUNCS)] = {};
    void *table = args->pAdapterFuncs;
    if (table) memcpy(savedTable, table, tableBytes);
    DWORD error = 0;
    HMODULE gpu = Load(path, &error);
    const HRESULT hr = gpu ? CallEntry(gpu, entry, args) : HRESULT_FROM_WIN32(error);
    if (FAILED(hr)) {
        *args = savedArgs;
        if (table) memcpy(table, savedTable, tableBytes);
    }
    return hr;
}

static HRESULT ForwardCpu(const Config &c, const char *entry, D3D10DDIARG_OPENADAPTER *args)
{
    DWORD error = 0;
    HMODULE cpu = Load(c.cpu, &error);
    return cpu ? CallEntry(cpu, entry, args) : HRESULT_FROM_WIN32(error);
}

static HRESULT ForwardApp(const Config &c, const wchar_t *image, const wchar_t *exe, const char *entry,
                          D3D10DDIARG_OPENADAPTER *args, size_t tableBytes)
{
    AppConfig a;
    ReadAppConfig(&a);
    const bool entry_10_2 = !strcmp(entry, "OpenAdapter10_2");
    wchar_t windows[PathChars];
    const UINT wn = GetSystemWindowsDirectoryW(windows, PathChars);
    if (!wn || wn >= PathChars) windows[0] = 0;
    const bool component = IsWindowsComponentPath(image, windows);
    const AppDecision d = DecideApp({exe, a.mode, a.allow, a.deny, entry_10_2, a.gpu[0] != 0, component});
    HRESULT gpuHr = S_FALSE; // not tried
    if (d.route == AppRoute::Gpu) {
        gpuHr = TryGpu(a.gpu, entry, args, tableBytes);
        if (SUCCEEDED(gpuHr)) {
            ReportApp(c, a, exe, entry, d, gpuHr, false, a.gpu, gpuHr);
            return gpuHr;
        }
    }
    const HRESULT hr = ForwardCpu(c, entry, args);
    ReportApp(c, a, exe, entry, d, gpuHr, d.route == AppRoute::Gpu, c.cpu, hr);
    return hr;
}

static HRESULT Forward(const char *entry, D3D10DDIARG_OPENADAPTER *args, size_t tableBytes)
{
    if (!args) return E_INVALIDARG;
    Config c;
    ReadConfig(&c);
    wchar_t exeBuffer[PathChars];
    exeBuffer[0] = 0;
    const wchar_t *exe = ExeBase(exeBuffer, PathChars); // exeBuffer keeps the full image path
    const Decision d = Decide({exe, c.clients, c.force_cpu, c.require_switches, c.blit_on, c.interop_on});
    if (d.reason == Reason::NotHostedClient) return ForwardApp(c, exeBuffer, exe, entry, args, tableBytes);
    // Desktop: dwm.exe and HostedClients, as router 5BBEB783.
    HRESULT hostedHr = S_FALSE; // not tried
    if (d.route == Route::Hosted) {
        hostedHr = TryGpu(c.hosted, entry, args, tableBytes);
        if (SUCCEEDED(hostedHr)) {
            Report(c, exe, entry, d, hostedHr, false, c.hosted, hostedHr);
            return hostedHr;
        }
    }
    const HRESULT hr = ForwardCpu(c, entry, args);
    Report(c, exe, entry, d, hostedHr, d.route == Route::Hosted, c.cpu, hr);
    return hr;
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *args)
{
    return Forward("OpenAdapter10", args, sizeof(D3D10DDI_ADAPTERFUNCS));
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *args)
{
    return Forward("OpenAdapter10_2", args, sizeof(D3D10_2DDI_ADAPTERFUNCS));
}
