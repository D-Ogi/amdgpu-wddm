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
//                                    (32-bit router: CpuUmdPathWow, default bc250d3d.dll next to this DLL)
//     HostedUmdPath       REG_SZ     hosted UMD, absolute path (default: bc250d3d_zink.dll next to this DLL)
//                                    (32-bit router: HostedUmdPathWow, same default)
//     DwmForceCpu         REG_DWORD  kill switch: any value other than 0 (or any other type) sends every hosted
//                                    client to the CPU UMD; absent = 0
//     RequireKmdSwitches  REG_DWORD  0 = ignore the KMD desktop switches; absent, 1 or any other value = require
//                                    both of them
//     HostedClients       REG_MULTI_SZ  further image base names routed like dwm.exe (test clients)
//     RouteLogDirectory   REG_SZ     when set: one line per OpenAdapter call to route-<exe>-<pid>.log there
//     DirectFlipFront     REG_DWORD  M15.14: non-zero puts the D3D11_1 front (front-adapter.h) in front of the
//                                    hosted UMD on the desktop route, so that the operating system can ask
//                                    pfnCheckDirectFlipSupport. Absent = on from this release, because the
//                                    front is finished and rides the desktop; 0 = off, which is the router that
//                                    shipped, byte for byte, and is this feature's bisect switch; any other
//                                    value kind = invalid, which is also off
//   HKLM\SOFTWARE\amdgpu-wddm\AppRouter       (applications only; never read for dwm.exe or a HostedClients entry)
//     Mode                REG_SZ     cpu | allowlist | gpu-default. Absent = cpu (the application kill switch); any
//                                    other string or value kind = invalid, which routes like cpu
//     GpuUmdPath          REG_SZ     application GPU UMD, absolute path; absent or not absolute = every application
//                                    on the CPU UMD (reason app-gpu-umd-unset). No compiled default.
//                                    (32-bit router: GpuUmdPathWow)
//     Allow               REG_MULTI_SZ  image base names routed to the GPU UMD in allowlist mode
//     Deny                REG_MULTI_SZ  image base names kept on the CPU UMD in both modes (Deny wins over Allow).
//                                    A Deny or Allow value that cannot be read (other kind, too long) makes the mode
//                                    invalid (fail safe: CPU).
//     RouteLogDirectory   REG_SZ     application route lines go there; absent = DesktopRouter's RouteLogDirectory
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics and Graphics\Applications\<image>   (applications only; app-settings-core.h)
//     RenderOnCpu         REG_DWORD  1 = the CPU UMD (reason app-render-on-cpu), checked after the protected list
//                                    and before Deny; the environment variable AMDGPU_WDDM_RENDER_ON_CPU wins over
//                                    the application key, which wins over the global key. It adds to Deny, which
//                                    keeps working as before; 0 never moves an application off the CPU UMD.
//   Built in: logonui.exe, consent.exe, lockapp.exe, credentialuibroker.exe and winlogon.exe always stay on the CPU
//   UMD (router-policy.h IsProtectedApp). OpenAdapter10 always goes to the CPU UMD: the application GPU UMD
//   exports OpenAdapter10_2 only. The Direct3D 10.0 runtime of Windows 11 does not call OpenAdapter10 when
//   OpenAdapter10_2 is exported: it opens this router through OpenAdapter10_2 and creates a D3D11-family device,
//   so a D3D10.0 application takes the AppRouter decision like a D3D11 one (BD-081, measured 2026-10-07).
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
//
// M15.14: in dwm.exe every desktop decision is also written to the session's desktop-route record
// (driver/contract/bc250_desktop_route.h): gpu, cpu-kill-switch, cpu-switches-off or cpu-fallback. The application
// shells read it before they ask for a scan-out primary, because the CPU compositor cannot read one, and a fallback
// to the CPU UMD is visible nowhere else. A HostedClients entry writes nothing: it is a test client, and its route
// is not the compositor's.

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
#include "router-identity.h"
#include "front-adapter.h"
#include "../app-settings/app-settings-core.h"
#include "../../contract/bc250_desktop_route.h"

#pragma comment(lib, "advapi32.lib")

using namespace bc250router;

#ifdef _WIN64
static const wchar_t DefaultCpuUmdPath[] = L"C:\\BC250\\m15\\desktop-umd173-007\\bc250d3d.dll";
static const wchar_t CpuUmdPathValue[] = L"CpuUmdPath";
static const wchar_t HostedUmdPathValue[] = L"HostedUmdPath";
static const wchar_t GpuUmdPathValue[] = L"GpuUmdPath";
static const REGSAM RegistryView = 0;
#else
// The 32-bit router (UserModeDriverNameWow, BD-064) reads the same keys and the same policy values as the 64-bit
// one: KEY_WOW64_64KEY keeps HKLM\SOFTWARE from being redirected to WOW6432Node, so Mode, Allow, Deny, DwmForceCpu
// and the rest have one copy for both. Only the three UMD paths differ, because a 32-bit process needs 32-bit UMDs:
// CpuUmdPathWow, HostedUmdPathWow and GpuUmdPathWow. An absent CpuUmdPathWow means bc250d3d.dll next to this DLL
// (there is no lab path to default to); an absent HostedUmdPathWow means bc250d3d_zink.dll next to this DLL, as in
// the 64-bit router. DWM is a 64-bit process, so the hosted route serves only 32-bit HostedClients entries.
static const wchar_t CpuUmdPathValue[] = L"CpuUmdPathWow";
static const wchar_t HostedUmdPathValue[] = L"HostedUmdPathWow";
static const wchar_t GpuUmdPathValue[] = L"GpuUmdPathWow";
static const wchar_t DefaultCpuUmdFile[] = L"bc250d3d.dll";
static const REGSAM RegistryView = KEY_WOW64_64KEY;
#endif
static const wchar_t RouterKey[] = L"SOFTWARE\\amdgpu-wddm\\DesktopRouter";
static const wchar_t AppRouterKey[] = L"SOFTWARE\\amdgpu-wddm\\AppRouter";
static const wchar_t KmdParametersKey[] = L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters";
static const wchar_t HostedUmdFile[] = L"bc250d3d_zink.dll";
static const size_t PathChars = 1024;
static const DWORD ClientsBytes = 4096;
static const DWORD AppListBytes = 16384;

// The DirectFlipFront request. An absent value means Requested from this release: the front is a finished
// feature and rides the desktop on, with the value 0 as the one switch that puts the shipped router back (the
// release-train rule, owner 2026-10-05). Off is the router that shipped: no front, no raised DDI, nothing to
// roll back. A value of another kind is Invalid and is still treated as off, because a desktop that cannot read
// its own switch must take the route that needs nothing of this release.
enum class Front { Off, Requested, Invalid };

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
    Front front;
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
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RouterKey, 0, KEY_QUERY_VALUE | RegistryView, &key) == ERROR_SUCCESS) {
        if (ReadString(key, CpuUmdPathValue, c->cpu, PathChars) && IsAbsolute(c->cpu)) c->cpu_source = "registry";
        else c->cpu[0] = 0;
        if (ReadString(key, HostedUmdPathValue, c->hosted, PathChars)) {
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
        switch (ReadDword(key, L"DirectFlipFront", &v)) {
        case Dword::Absent: c->front = Front::Requested; break;
        case Dword::Value: c->front = v ? Front::Requested : Front::Off; break;
        case Dword::Other: c->front = Front::Invalid; break;
        }
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
#ifdef _WIN64
        wcscpy_s(c->cpu, DefaultCpuUmdPath);
        c->cpu_source = "default";
#else
        wchar_t dir[PathChars];
        ModuleDirectory(dir, PathChars);
        if (dir[0] && _snwprintf_s(c->cpu, PathChars, _TRUNCATE, L"%ls\\%ls", dir, DefaultCpuUmdFile) > 0)
            c->cpu_source = "router-directory";
        else {
            c->cpu[0] = 0;
            c->cpu_source = "unavailable";
        }
#endif
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
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KmdParametersKey, 0, KEY_QUERY_VALUE | RegistryView, &key) == ERROR_SUCCESS) {
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
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AppRouterKey, 0, KEY_QUERY_VALUE | RegistryView, &key) != ERROR_SUCCESS) return;
    wchar_t text[64];
    switch (ReadText(key, L"Mode", RRF_RT_REG_SZ, text, sizeof(text))) {
    case Text::Absent: a->mode = AppMode::Cpu; a->mode_source = "absent"; break;
    case Text::Value:
        a->mode = ParseAppMode(text);
        a->mode_source = a->mode == AppMode::Invalid ? "registry-invalid" : "registry";
        break;
    case Text::Other: a->mode = AppMode::Invalid; a->mode_source = "registry-invalid"; break;
    }
    switch (ReadText(key, GpuUmdPathValue, RRF_RT_REG_SZ, a->gpu, sizeof(a->gpu))) {
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

// The desktop line. Every field of router 5BBEB783 is in the same place with the same spelling, and M15.14
// appends columns at the end only (front=, then record=), so the kit's parser keeps working and an older log and
// a newer one differ by the last fields.
static void Report(const Config &c, const wchar_t *exe, const char *entry, const Decision &d, HRESULT hostedHr,
                   bool fellBack, const wchar_t *module, HRESULT hr, const char *front, const char *record)
{
    char line[2048];
    int n = _snprintf_s(line, _TRUNCATE,
        "bc250d3d_router pid=%lu exe=%ls entry=%s route=%s reason=%s fallback=%u hosted_hr=%08lx hr=%08lx module=%ls "
        "cpu_source=%s hosted_source=%s force_cpu=%lu require_switches=%lu switch_source=%s blit=%u interop=%u "
        "front=%s record=%s\n",
        GetCurrentProcessId(), exe, entry, d.route == Route::Hosted && !fellBack ? "hosted" : "cpu",
        ReasonName(d.reason), fellBack ? 1u : 0u, (unsigned long)hostedHr, (unsigned long)hr, module,
        c.cpu_source, c.hosted_source, c.force_cpu, c.require_switches, c.switch_source, c.blit_on ? 1u : 0u,
        c.interop_on ? 1u : 0u, front, record);
    if (n < 0) n = (int)strlen(line);
    WriteRouteLine(c.log_directory, exe, line, n);
}

// The desktop-route record of this compositor (driver/contract/bc250_desktop_route.h), made at the first desktop
// decision of dwm.exe and kept, with its section handle, until the process ends. A record that cannot be made is
// tried again at the next decision; until then the shells read no record and stay composed. What the record
// column says:
//   none           not dwm.exe (a HostedClients test client): nothing written
//   published      this decision is in the record
//   failed-<n>     the section could not be made or mapped, Win32 error n
static SRWLOCK RouteRecordLock = SRWLOCK_INIT;
static bc250_desktop_route *RouteRecord;

static const char *PublishRoute(const wchar_t *exe, unsigned route, HRESULT hostedHr, char *word, size_t chars)
{
    if (_wcsicmp(exe, L"dwm.exe")) return "none";
    DWORD error = 0;
    AcquireSRWLockExclusive(&RouteRecordLock);
    if (!RouteRecord) RouteRecord = bc250_desktop_route_create(BC250_DESKTOP_ROUTE_NAME, &error);
    if (RouteRecord) bc250_desktop_route_store(RouteRecord, route, GetCurrentProcessId(), (unsigned)hostedHr);
    const bool published = RouteRecord != nullptr;
    ReleaseSRWLockExclusive(&RouteRecordLock);
    if (published) return "published";
    _snprintf_s(word, chars, _TRUNCATE, "failed-%lu", error);
    return word;
}

// The record's word for a desktop decision that ended on the CPU UMD.
static unsigned CpuRouteWord(const Decision &d)
{
    if (d.route == Route::Hosted) return BC250_DESKTOP_ROUTE_FALLBACK; // the hosted open failed
    return d.reason == Reason::KillSwitch ? BC250_DESKTOP_ROUTE_KILL_SWITCH : BC250_DESKTOP_ROUTE_SWITCHES_OFF;
}

// What the front column says, and what the words mean:
//   off          DirectFlipFront is zero. The hosted UMD's own D3D10.0 table reached the runtime.
//   invalid      the value is of another kind. Treated as off; the word says the switch could not be read.
//   on           the front is installed over the hosted adapter table and the runtime may offer D3D11_1.
//   unavailable  the front was asked for and is NOT in the path: the D3D10.0 adapter entry (the front needs
//                the 10_2 table), an incomplete or missing hosted adapter table, no free adapter slot, or a
//                route that ended on the CPU UMD. The desktop then behaves as it did with front=off.
static const char *InstallFront(const Config &c, const char *entry, D3D10DDIARG_OPENADAPTER *args,
                                const wchar_t *exe)
{
    if (c.front == Front::Invalid) return "invalid";
    if (c.front != Front::Requested) return "off";
    if (strcmp(entry, "OpenAdapter10_2")) return "unavailable";
    return bc250front::Install(args, c.log_directory, exe) ? "on" : "unavailable";
}

// The application line: the same leading fields, gpu_hr in place of hosted_hr, then the AppRouter state, then the
// RenderOnCpu setting ("unset" or <value>/<source>).
static void ReportApp(const Config &c, const AppConfig &a, const wchar_t *exe, const char *entry, const AppDecision &d,
                      HRESULT gpuHr, bool fellBack, const wchar_t *module, HRESULT hr,
                      const amdgpu_wddm::app_settings::Value &renderOnCpu)
{
    char setting[48] = "unset";
    if (renderOnCpu.set())
        _snprintf_s(setting, _TRUNCATE, "%u/%s", renderOnCpu.value,
                    amdgpu_wddm::app_settings::source_name(renderOnCpu.source));
    char line[2048];
    int n = _snprintf_s(line, _TRUNCATE,
        "bc250d3d_router pid=%lu exe=%ls entry=%s route=%s reason=%s fallback=%u gpu_hr=%08lx hr=%08lx module=%ls "
        "app_mode=%s mode_source=%s gpu_source=%s cpu_source=%s render_on_cpu=%s\n",
        GetCurrentProcessId(), exe, entry, d.route == AppRoute::Gpu && !fellBack ? "gpu" : "cpu",
        AppReasonName(d.reason), fellBack ? 1u : 0u, (unsigned long)gpuHr, (unsigned long)hr, module,
        AppModeName(a.mode), a.mode_source, a.gpu_source, c.cpu_source, setting);
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
    if (!wn || wn >= PathChars) windows[0] = 0; // ClassifyComponent: Unknown, which keeps gpu-default on the CPU UMD
    const Component component = ClassifyComponent(image, windows);
    // Read at every call, as the AppRouter key is (the shells read the other settings once per process).
    namespace as = amdgpu_wddm::app_settings;
    as::Settings settings;
    as::resolve(exe, as::system_sources(), settings);
    const as::Value renderOnCpu = settings[as::Setting::RenderOnCpu];
    const AppDecision d = DecideApp({exe, a.mode, a.allow, a.deny, entry_10_2, a.gpu[0] != 0, component,
                                     as::render_on_cpu(settings)});
    HRESULT gpuHr = S_FALSE; // not tried
    if (d.route == AppRoute::Gpu) {
        gpuHr = TryGpu(a.gpu, entry, args, tableBytes);
        if (SUCCEEDED(gpuHr)) {
            ReportApp(c, a, exe, entry, d, gpuHr, false, a.gpu, gpuHr, renderOnCpu);
            return gpuHr;
        }
    }
    const HRESULT hr = ForwardCpu(c, entry, args);
    ReportApp(c, a, exe, entry, d, gpuHr, d.route == AppRoute::Gpu, c.cpu, hr, renderOnCpu);
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
    char recordWord[32];
    if (d.route == Route::Hosted) {
        hostedHr = TryGpu(c.hosted, entry, args, tableBytes);
        if (SUCCEEDED(hostedHr)) {
            // The front goes on only after the hosted open succeeded, so the hosted table it saves is the
            // real one and a failed hosted open still restores the caller's table untouched (TryGpu).
            const char *front = InstallFront(c, entry, args, exe);
            Report(c, exe, entry, d, hostedHr, false, c.hosted, hostedHr, front,
                   PublishRoute(exe, BC250_DESKTOP_ROUTE_GPU, hostedHr, recordWord, sizeof(recordWord)));
            return hostedHr;
        }
    }
    const HRESULT hr = ForwardCpu(c, entry, args);
    Report(c, exe, entry, d, hostedHr, d.route == Route::Hosted, c.cpu, hr,
           c.front == Front::Invalid ? "invalid" : c.front == Front::Requested ? "unavailable" : "off",
           PublishRoute(exe, CpuRouteWord(d), hostedHr, recordWord, sizeof(recordWord)));
    return hr;
}

#ifdef _WIN64
#define UMD_EXPORT __declspec(dllexport)
#else
// x86: __stdcall decorates an exported name (_OpenAdapter10@4), and the D3D runtime asks GetProcAddress for the
// plain one. The linker exports the plain names instead (BD-064).
#define UMD_EXPORT
#pragma comment(linker, "/EXPORT:OpenAdapter10=_OpenAdapter10@4")
#pragma comment(linker, "/EXPORT:OpenAdapter10_2=_OpenAdapter10_2@4")
#endif
extern "C" UMD_EXPORT HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *args)
{
    return Forward("OpenAdapter10", args, sizeof(D3D10DDI_ADAPTERFUNCS));
}

extern "C" UMD_EXPORT HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *args)
{
    return Forward("OpenAdapter10_2", args, sizeof(D3D10_2DDI_ADAPTERFUNCS));
}
