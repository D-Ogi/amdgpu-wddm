// SPDX-License-Identifier: MIT
//
// Host tests of bc250d3d_router.dll (desktop route and AppRouter application policy), of the registered hosted UMD
// bc250d3d_zink.dll and of the router in front of the DXVK shell amdgpu_wddm_d3d11.dll, without a device.
//
//   test-router.exe run <layout> <out>        every scenario, each in a fresh child process
//   test-router.exe child <scenario> <layout> one scenario (used by run)
//
// No machine state is touched: each child loads a private application hive from <layout>\hives
// (RegLoadAppKey) and overrides HKEY_LOCAL_MACHINE with it for the process (RegOverridePredefKey), so the
// router's and the UMD's HKLM reads see only the scenario's keys. The runtime's adapter callback is a double
// that returns the KMD's UMDRIVERPRIVATE identity trailer (or not). Layout: run-host-tests.ps1.

#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#define D3D10DDI_MINOR_HEADER_VERSION 2
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#pragma warning(pop)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <share.h>
#include <string>
#include <vector>
#include "../router-policy.h"
#include "../router-identity.h"
#include "../front-direct-flip.h"   // the front: front-adapter.h, front-resource.h and the pure flip rule
#include "bc250_adapter_identity.h" // driver/contract (build.ps1 /I)
#include "bc250_scanout_caps.h"

#pragma comment(lib, "advapi32.lib")

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("  check failed line %d: %s: ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------- policy (pure)

static void PolicyTests()
{
    using namespace bc250router;
    const wchar_t clients[] = L"gpu-window-control.exe\0Probe.EXE\0";
    struct Case { const wchar_t *exe; const wchar_t *clients; unsigned long force, require; bool blit, interop; Route route; Reason reason; };
    const Case cases[] = {
        {L"dwm.exe", nullptr, 0, 1, true, true, Route::Hosted, Reason::Hosted},
        {L"DWM.EXE", nullptr, 0, 1, true, true, Route::Hosted, Reason::Hosted},
        {L"dwm.exe", nullptr, 1, 1, true, true, Route::Cpu, Reason::KillSwitch},
        {L"dwm.exe", nullptr, 0xFFFFFFFF, 0, true, true, Route::Cpu, Reason::KillSwitch},
        {L"dwm.exe", nullptr, 0, 1, true, false, Route::Cpu, Reason::SwitchesOff},
        {L"dwm.exe", nullptr, 0, 1, false, true, Route::Cpu, Reason::SwitchesOff},
        {L"dwm.exe", nullptr, 0, 1, false, false, Route::Cpu, Reason::SwitchesOff},
        {L"dwm.exe", nullptr, 0, 0, false, false, Route::Hosted, Reason::Hosted},
        {L"explorer.exe", nullptr, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {L"dwm.exe.exe", nullptr, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {L"xdwm.exe", nullptr, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {L"dwm", nullptr, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {L"", nullptr, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {nullptr, nullptr, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {L"probe.exe", clients, 0, 1, true, true, Route::Hosted, Reason::Hosted},
        {L"gpu-window-control.exe", clients, 1, 1, true, true, Route::Cpu, Reason::KillSwitch},
        {L"other.exe", clients, 0, 1, true, true, Route::Cpu, Reason::NotHostedClient},
        {L"dwm.exe", clients, 0, 1, true, true, Route::Hosted, Reason::Hosted},
    };
    for (const Case &c : cases) {
        Decision d = Decide({c.exe, c.clients, c.force, c.require, c.blit, c.interop});
        CHECK(d.route == c.route && d.reason == c.reason, "exe=%ls force=%lu require=%lu blit=%d interop=%d got %s",
              c.exe ? c.exe : L"(null)", c.force, c.require, c.blit, c.interop, ReasonName(d.reason));
    }

    // Application policy.
    const wchar_t allow[] = L"steamwebhelper.exe\0D3D11MT.EXE\0d3d11bench.exe\0";
    const wchar_t deny[] = L"witcher3.exe\0d3d11bench.exe\0";
    const wchar_t protectedAllowed[] = L"logonui.exe\0consent.exe\0lockapp.exe\0credentialuibroker.exe\0winlogon.exe\0";
    struct AppCase { const wchar_t *exe; AppMode mode; const wchar_t *allow, *deny; bool e102, gpu; AppRoute route; AppReason reason; };
    const AppMode Al = AppMode::Allowlist, Gd = AppMode::GpuDefault;
    const AppCase app[] = {
        {L"d3d11mt.exe", Al, allow, nullptr, true, true, AppRoute::Gpu, AppReason::Allowed},
        {L"D3D11mt.Exe", Al, allow, deny, true, true, AppRoute::Gpu, AppReason::Allowed},
        {L"steamwebhelper.exe", Al, allow, deny, true, true, AppRoute::Gpu, AppReason::Allowed},
        {L"notepad.exe", Al, allow, deny, true, true, AppRoute::Cpu, AppReason::NotAllowed},
        {L"d3d11mt", Al, allow, nullptr, true, true, AppRoute::Cpu, AppReason::NotAllowed},
        {L"xd3d11mt.exe", Al, allow, nullptr, true, true, AppRoute::Cpu, AppReason::NotAllowed},
        {L"d3d11mt.exe", Al, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::NotAllowed},
        {L"d3d11bench.exe", Al, allow, deny, true, true, AppRoute::Cpu, AppReason::Denied},
        {L"notepad.exe", Gd, nullptr, nullptr, true, true, AppRoute::Gpu, AppReason::Default},
        {L"notepad.exe", Gd, allow, deny, true, true, AppRoute::Gpu, AppReason::Default},
        {L"WITCHER3.EXE", Gd, allow, deny, true, true, AppRoute::Cpu, AppReason::Denied},
        {L"d3d11bench.exe", Gd, allow, deny, true, true, AppRoute::Cpu, AppReason::Denied},
        {L"d3d11mt.exe", AppMode::Cpu, allow, nullptr, true, true, AppRoute::Cpu, AppReason::ModeCpu},
        {L"d3d11mt.exe", AppMode::Invalid, allow, nullptr, true, true, AppRoute::Cpu, AppReason::ModeInvalid},
        {L"d3d11mt.exe", (AppMode)17, allow, nullptr, true, true, AppRoute::Cpu, AppReason::ModeInvalid},
        {L"d3d11mt.exe", Al, allow, nullptr, false, true, AppRoute::Cpu, AppReason::D3d10Entry},
        {L"notepad.exe", Gd, nullptr, nullptr, false, true, AppRoute::Cpu, AppReason::D3d10Entry},
        // BD-081: the Direct3D 10.0 runtime of Windows 11 opens the router through OpenAdapter10_2 (measured
        // 2026-10-07), so a D3D10.0 application is decided at e102=true: the GPU UMD under gpu-default and for an
        // allowed image, the CPU UMD for a denied or a not-allowed one, exactly as a D3D11 application.
        {L"d3d10probe.exe", Gd, nullptr, nullptr, true, true, AppRoute::Gpu, AppReason::Default},
        {L"d3d10probe.exe", Al, L"d3d10probe.exe\0", nullptr, true, true, AppRoute::Gpu, AppReason::Allowed},
        {L"d3d10probe.exe", Al, allow, nullptr, true, true, AppRoute::Cpu, AppReason::NotAllowed},
        {L"d3d10probe.exe", Gd, nullptr, L"d3d10probe.exe\0", true, true, AppRoute::Cpu, AppReason::Denied},
        {L"d3d11mt.exe", Al, allow, nullptr, true, false, AppRoute::Cpu, AppReason::GpuUmdUnset},
        {L"notepad.exe", Gd, nullptr, nullptr, true, false, AppRoute::Cpu, AppReason::GpuUmdUnset},
        {L"logonui.exe", Gd, protectedAllowed, nullptr, true, true, AppRoute::Cpu, AppReason::Protected},
        {L"LogonUI.exe", Al, protectedAllowed, nullptr, true, true, AppRoute::Cpu, AppReason::Protected},
        {L"consent.exe", Gd, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::Protected},
        {L"lockapp.exe", Gd, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::Protected},
        {L"CredentialUIBroker.exe", Gd, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::Protected},
        {L"winlogon.exe", Gd, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::Protected},
        {L"", Gd, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::NoExe},
        {nullptr, Gd, nullptr, nullptr, true, true, AppRoute::Cpu, AppReason::NoExe},
    };
    for (const AppCase &c : app) {
        AppDecision d = DecideApp({c.exe, c.mode, c.allow, c.deny, c.e102, c.gpu});
        CHECK(d.route == c.route && d.reason == c.reason, "app exe=%ls mode=%d e102=%d gpu=%d got %s",
              c.exe ? c.exe : L"(null)", (int)c.mode, c.e102, c.gpu, AppReasonName(d.reason));
    }

    // Windows components take the GPU UMD under gpu-default since train b20 (BD-061 verified, BD-088); Deny and the
    // protected list still win.
    const wchar_t allowDx[] = L"dxdiag.exe\0";
    // Unknown (an unresolvable image or Windows directory) stays on the CPU UMD unless Allow names the image.
    const Component Y = Component::Yes, N = Component::No, U = Component::Unknown;
    struct CompCase { const wchar_t *exe; AppMode mode; const wchar_t *allow, *deny; Component component; AppRoute route; AppReason reason; };
    const CompCase comp[] = {
        {L"notepad.exe", Gd, allowDx, nullptr, Y, AppRoute::Gpu, AppReason::Default},
        {L"dxdiag.exe", Gd, allowDx, nullptr, Y, AppRoute::Gpu, AppReason::Default},
        {L"game.exe", Gd, allowDx, nullptr, N, AppRoute::Gpu, AppReason::Default},
        {L"witcher3.exe", Gd, allowDx, deny, N, AppRoute::Cpu, AppReason::Denied},
        {L"dxdiag.exe", Gd, allowDx, L"dxdiag.exe\0", Y, AppRoute::Cpu, AppReason::Denied},
        {L"logonui.exe", Gd, protectedAllowed, nullptr, Y, AppRoute::Cpu, AppReason::Protected},
        {L"notepad.exe", Al, allowDx, nullptr, Y, AppRoute::Cpu, AppReason::NotAllowed},
        {L"dxdiag.exe", Al, allowDx, nullptr, Y, AppRoute::Gpu, AppReason::Allowed},
        {L"game.exe", Gd, allowDx, nullptr, U, AppRoute::Cpu, AppReason::ComponentUnknown},
        {L"dxdiag.exe", Gd, allowDx, nullptr, U, AppRoute::Gpu, AppReason::Default},
        {L"dxdiag.exe", Gd, allowDx, L"dxdiag.exe\0", U, AppRoute::Cpu, AppReason::Denied},
        {L"logonui.exe", Gd, protectedAllowed, nullptr, U, AppRoute::Cpu, AppReason::Protected},
        {L"game.exe", Al, L"game.exe\0", nullptr, U, AppRoute::Gpu, AppReason::Allowed},
    };
    for (const CompCase &c : comp) {
        AppDecision d = DecideApp({c.exe, c.mode, c.allow, c.deny, true, true, c.component});
        CHECK(d.route == c.route && d.reason == c.reason, "component exe=%ls mode=%d comp=%d got %s",
              c.exe, (int)c.mode, (int)c.component, AppReasonName(d.reason));
    }
    struct PathCase { const wchar_t *image, *windows; bool component; };
    const PathCase paths[] = {
        {L"C:\\Windows\\System32\\notepad.exe", L"C:\\Windows", true},
        {L"c:\\windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe", L"C:\\Windows\\", true},
        {L"C:\\Windows\\explorer.exe", L"C:\\Windows", true},
        {L"C:\\Windows2\\game.exe", L"C:\\Windows", false},
        {L"C:\\Windows.old\\game.exe", L"C:\\Windows", false},
        {L"C:\\WindowsGames\\game.exe", L"C:\\Windows", false},
        {L"C:\\Program Files\\WindowsApps\\Microsoft.WindowsNotepad_11.2507.26.0_x64__8wekyb3d8bbwe\\Notepad\\Notepad.exe", L"C:\\Windows", true},
        {L"C:\\Program Files\\WindowsApps\\Microsoft.WindowsCalculator_11.2502.2.0_x64__8wekyb3d8bbwe\\CalculatorApp.exe", L"C:\\Windows", true},
        {L"C:\\Program Files\\WindowsApps\\MicrosoftWindows.Client.CBS_1000.0_x64__cw5n1h2txyewy\\x.exe", L"C:\\Windows", true},
        {L"D:\\WindowsApps\\Microsoft.Foo_1.0_x64__8wekyb3d8bbwe\\foo.exe", L"C:\\Windows", true},
        {L"C:\\Program Files\\WindowsApps\\BethesdaSoftworks.Game_1.0_x64__3275kfvn8vcwc\\game.exe", L"C:\\Windows", false},
        {L"C:\\XboxGames\\Game\\Content\\game.exe", L"C:\\Windows", false},
        {L"D:\\SteamLibrary\\steamapps\\common\\Factorio\\bin\\x64\\factorio.exe", L"C:\\Windows", false},
        {L"C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe", L"C:\\Windows", false},
        {L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", L"C:\\Windows", false},
        {L"C:\\Games\\MicrosoftWindowsApps\\game.exe", L"C:\\Windows", false},
        {L"C:\\Windows\\notepad.exe", L"", false},
        {L"", L"C:\\Windows", false},
        {nullptr, L"C:\\Windows", false},
    };
    for (const PathCase &p : paths)
        CHECK(IsWindowsComponentPath(p.image, p.windows) == p.component, "IsWindowsComponentPath(%ls, %ls) != %d",
              p.image ? p.image : L"(null)", p.windows, p.component);
    struct ModeCase { const wchar_t *text; AppMode mode; };
    const ModeCase modes[] = {
        {L"cpu", AppMode::Cpu}, {L"CPU", AppMode::Cpu}, {L"allowlist", AppMode::Allowlist}, {L"AllowList", AppMode::Allowlist},
        {L"gpu-default", AppMode::GpuDefault}, {L"GPU-Default", AppMode::GpuDefault},
        {L"gpu", AppMode::Invalid}, {L"gpu_default", AppMode::Invalid}, {L"", AppMode::Invalid}, {L" cpu", AppMode::Invalid},
        {L"allowlist ", AppMode::Invalid}, {nullptr, AppMode::Invalid},
    };
    for (const ModeCase &m : modes)
        CHECK(ParseAppMode(m.text) == m.mode, "ParseAppMode(%ls) = %s", m.text ? m.text : L"(null)", AppModeName(ParseAppMode(m.text)));
}

// ---------------------------------------------------------------- registry double

static HKEY Root;
static std::wstring Layout;

static HKEY Sub(const wchar_t *path)
{
    HKEY k = nullptr;
    LSTATUS s = RegCreateKeyExW(Root, path, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
    if (s != ERROR_SUCCESS) { printf("  RegCreateKeyEx %ls failed %ld\n", path, s); ExitProcess(3); }
    return k;
}
static const wchar_t RouterKey[] = L"SOFTWARE\\amdgpu-wddm\\DesktopRouter";
static const wchar_t HostedKey[] = L"SOFTWARE\\amdgpu-wddm\\HostedUmd";
static const wchar_t KmdKey[] = L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters";
static const wchar_t AppKey[] = L"SOFTWARE\\amdgpu-wddm\\AppRouter";
// The UMD path values of the router under test and of the other bitness (router.cpp, BD-064): a 32-bit router reads
// CpuUmdPathWow, HostedUmdPathWow and GpuUmdPathWow and must ignore the 64-bit names, and the other way round.
#ifdef _WIN64
static const wchar_t CpuUmdPathName[] = L"CpuUmdPath", HostedUmdPathName[] = L"HostedUmdPath", GpuUmdPathName[] = L"GpuUmdPath";
static const wchar_t OtherCpuUmdPathName[] = L"CpuUmdPathWow", OtherGpuUmdPathName[] = L"GpuUmdPathWow";
#else
static const wchar_t CpuUmdPathName[] = L"CpuUmdPathWow", HostedUmdPathName[] = L"HostedUmdPathWow", GpuUmdPathName[] = L"GpuUmdPathWow";
static const wchar_t OtherCpuUmdPathName[] = L"CpuUmdPath", OtherGpuUmdPathName[] = L"GpuUmdPath";
#endif

static void SetSz(const wchar_t *key, const wchar_t *name, const std::wstring &v)
{
    HKEY k = Sub(key);
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}
static void SetDw(const wchar_t *key, const wchar_t *name, DWORD v)
{
    HKEY k = Sub(key);
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}
static void SetMulti(const wchar_t *key, const wchar_t *name, const std::vector<std::wstring> &items)
{
    std::wstring block;
    for (auto &i : items) { block += i; block.push_back(0); }
    block.push_back(0);
    HKEY k = Sub(key);
    RegSetValueExW(k, name, 0, REG_MULTI_SZ, (const BYTE *)block.data(), (DWORD)(block.size() * sizeof(wchar_t)));
    RegCloseKey(k);
}
static void Switches(bool blit, bool interop)
{
    SetDw(KmdKey, L"EnableGpuPresentBlit", blit ? 1 : 0);
    SetDw(KmdKey, L"EnableCddDwmInterop", interop ? 1 : 0);
}
// KMD 0.7.181+: the start-latched state, effective | requested << 8.
static void Latched(DWORD state) { SetDw(KmdKey, L"InteropLastState", state); }

static void OpenHive(const std::string &scenario)
{
    std::wstring path = Layout + L"\\hives\\" + std::wstring(scenario.begin(), scenario.end()) + L".dat";
    LSTATUS s = RegLoadAppKeyW(path.c_str(), &Root, KEY_ALL_ACCESS, 0, 0);
    if (s != ERROR_SUCCESS) { printf("  RegLoadAppKey failed %ld\n", s); ExitProcess(3); }
}
static void OverrideHklm()
{
    LSTATUS s = RegOverridePredefKey(HKEY_LOCAL_MACHINE, Root);
    if (s != ERROR_SUCCESS) { printf("  RegOverridePredefKey failed %ld\n", s); ExitProcess(3); }
}

// ---------------------------------------------------------------- runtime double

enum class Query { Trailer, NoTrailer, Fail, BadVersion, ZeroLuid, BadReserved };
static Query QueryMode = Query::Trailer;
// M15.14 increment 2: the scan-out caps trailer the double writes behind the identity, as the kernel driver
// does when EnableDirectFlipHandshake and every flip-path gate are on. Off (all zero) by default, which is
// what a kernel driver without the trailer and a start with the switch off both write. A scenario sets the
// three fields; ScanoutFlags 0 with a geometry is the torn shape a reader must refuse as well.
static bool ScanoutTrailer;
static unsigned ScanoutFlags = BC250_SCANOUT_CAPS_DIRECT_FLIP;
static unsigned ScanoutWidth, ScanoutHeight;
static UINT64 QueryLuid = 0x0000000100002A5FULL;
static unsigned QueryCalls;
static UINT QuerySize;
static HANDLE QueryHandle;

// The adapter query sizes a reader may legitimately ask for. The identity trailer alone is
// BC250_ADAPTER_CAPS_BYTES; a reader that also reads the M15.14 scan-out caps trailer must size its buffer
// BC250_SCANOUT_CAPS_TOTAL, because the kernel driver writes the trailer only into a buffer long enough to
// hold it (driver/contract/bc250_scanout_caps.h). Both are right, so the gate accepts either and the message
// names the one it saw.
static bool CapsQuerySize(UINT bytes)
{
    return bytes == BC250_ADAPTER_CAPS_BYTES || bytes == BC250_SCANOUT_CAPS_TOTAL;
}

static HRESULT APIENTRY FakeQueryAdapterInfo(HANDLE adapter, const D3DDDICB_QUERYADAPTERINFO *q)
{
    ++QueryCalls;
    QueryHandle = adapter;
    QuerySize = q->PrivateDriverDataSize;
    if (QueryMode == Query::Fail) return E_FAIL;
    if (QueryMode == Query::NoTrailer || q->PrivateDriverDataSize < BC250_ADAPTER_CAPS_BYTES) return S_OK;
    bc250_adapter_identity t = {BC250_ADAPTER_IDENTITY_MAGIC, BC250_ADAPTER_IDENTITY_VERSION, sizeof(t),
                                (unsigned)QueryLuid, (unsigned)(QueryLuid >> 32), 0};
    if (QueryMode == Query::BadVersion) t.version = 2;
    if (QueryMode == Query::ZeroLuid) t.luid_low = t.luid_high = 0;
    if (QueryMode == Query::BadReserved) t.reserved = 1;
    memcpy((BYTE *)q->pPrivateDriverData + BC250_ADAPTER_IDENTITY_OFFSET, &t, sizeof(t));
    // Whole or not at all, and only into a buffer that holds all of it (driver/kmd/wddm.c).
    if (ScanoutTrailer && q->PrivateDriverDataSize >= BC250_SCANOUT_CAPS_TOTAL) {
        bc250_scanout_caps c = {BC250_SCANOUT_CAPS_MAGIC, BC250_SCANOUT_CAPS_VERSION, sizeof(c), ScanoutFlags,
                                ScanoutWidth, ScanoutHeight};
        memcpy((BYTE *)q->pPrivateDriverData + BC250_SCANOUT_CAPS_OFFSET, &c, sizeof(c));
    }
    return S_OK;
}

struct Opened {
    HRESULT hr;
    UINT_PTR tag;
    D3D10_2DDI_ADAPTERFUNCS funcs;
    D3D10DDI_HADAPTER adapter;
};

static Opened Open(HMODULE m, const char *entry)
{
    Opened o = {};
    static D3DDDI_ADAPTERCALLBACKS callbacks;
    callbacks.pfnQueryAdapterInfoCb = (PFND3DDDI_QUERYADAPTERINFOCB)FakeQueryAdapterInfo;
    D3D10DDIARG_OPENADAPTER a = {};
    a.hRTAdapter.handle = (HANDLE)(UINT_PTR)0x5A5A;
    a.Interface = D3D10_0_DDI_INTERFACE_VERSION;
    a.Version = D3D10_0_DDI_SUPPORTED & 0xFFFFFFFF;
    a.pAdapterCallbacks = &callbacks;
    a.pAdapterFuncs_2 = &o.funcs;
    auto fn = (HRESULT(APIENTRY *)(D3D10DDIARG_OPENADAPTER *))GetProcAddress(m, entry);
    if (!fn) { o.hr = E_NOINTERFACE; return o; }
    o.hr = fn(&a);
    o.tag = (UINT_PTR)a.hAdapter.pDrvPrivate;
    o.adapter = a.hAdapter;
    return o;
}

static HMODULE LoadAt(const std::wstring &relative)
{
    std::wstring p = Layout + L"\\" + relative;
    HMODULE m = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { printf("  LoadLibrary %ls failed %lu\n", p.c_str(), GetLastError()); ExitProcess(3); }
    return m;
}
static bool Loaded(const std::wstring &relative) { return GetModuleHandleW((Layout + L"\\" + relative).c_str()) != nullptr; }
// The module the ROUTER loaded, never loaded again here: the front scenarios ask the hosted double what it
// recorded, and a second LoadLibrary of the same path would be the same module anyway.
static HMODULE ModuleAt(const std::wstring &relative) { return GetModuleHandleW((Layout + L"\\" + relative).c_str()); }
static bool Exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string ReadAll(const std::wstring &path)
{
    std::string s;
    FILE *f = nullptr;
    // Shared read: the UMD still holds its log open for writing, as a reader on the lab would find it.
    f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
static std::wstring ExeBase()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t *b = wcsrchr(exe, L'\\');
    return b ? b + 1 : exe;
}
static std::wstring RouteLog(const std::wstring &dir)
{
    return dir + L"\\route-" + ExeBase() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log";
}

// Tags of the fake UMDs (fake-umd.cpp): CPU 0xC0, hosted 0x60, application GPU 0xA0.
static const UINT_PTR CpuTag102 = 0xC0 * 1000 + 102, CpuTag10 = 0xC0 * 1000 + 10;
static const UINT_PTR HostedTag102 = 0x60 * 1000 + 102, HostedTag10 = 0x60 * 1000 + 10;
static const UINT_PTR AppTag102 = 0xA0 * 1000 + 102;

static std::wstring Upper(std::wstring s) { for (auto &c : s) c = (wchar_t)towupper(c); return s; }
static void DeleteValue(const wchar_t *key, const wchar_t *name)
{
    HKEY k = Sub(key);
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}
// Application policy: the given mode, the application double as GPU UMD (relative to the layout), lines to applogs.
static void AppBase(const wchar_t *mode, const wchar_t *gpu = L"app\\amdgpu_wddm_d3d11.dll")
{
    if (mode) SetSz(AppKey, L"Mode", mode);
    if (gpu) SetSz(AppKey, GpuUmdPathName, Layout + L"\\" + gpu);
    SetSz(AppKey, L"RouteLogDirectory", Layout + L"\\applogs");
}
static std::string AppLog() { return ReadAll(RouteLog(Layout + L"\\applogs")); }
static bool Has(const std::string &log, const char *text) { return log.find(text) != std::string::npos; }

// A handle as the front and the doubles print it, with printf's %p. The width is the pointer width of the
// build: 16 hex digits for x64, 8 for x86. An expected line must be composed, never written out, or the gate
// reads a 64-bit tree only (the x86 router gate failed front-on on exactly that).
static std::string Ptr(UINT_PTR value)
{
    char text[32];
    sprintf_s(text, "%p", (void *)value);
    return text;
}

// Common router key: fake CPU UMD, hosted UMD from the router's own directory (no HostedUmdPath).
static void RouterBase(bool clientIsSelf)
{
    SetSz(RouterKey, CpuUmdPathName, Layout + L"\\cpu\\bc250d3d.dll");
    SetDw(RouterKey, L"DwmForceCpu", 0);
    SetDw(RouterKey, L"RequireKmdSwitches", 1);
    if (clientIsSelf) SetMulti(RouterKey, L"HostedClients", {L"unrelated.exe", ExeBase()});
}

// ---------------------------------------------------------------- image identity (router-identity.h)

// ClassifyComponent on real files: the spellings GetModuleFileNameW may return for one file must classify alike.
// Fixtures live in <layout>\identity (junctions made with mklink /J, which needs no privilege).
static void IdentityTests()
{
    using namespace bc250router;
    wchar_t windows[MAX_PATH];
    const UINT wn = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    if (!wn || wn >= MAX_PATH) { CHECK(false, "GetSystemWindowsDirectoryW failed"); return; }
    const std::wstring win = windows;
    const std::wstring cmdExe = win + L"\\System32\\cmd.exe";
    const std::wstring base = Layout + L"\\identity";
    CreateDirectoryW(base.c_str(), nullptr);

    struct Case { std::wstring image, windows_dir; Component want; const char *what; };
    std::vector<Case> cases = {
        {cmdExe, win, Component::Yes, "plain System32 image"},
        {L"\\\\?\\" + cmdExe, win, Component::Yes, "extended-prefix image"},
        {cmdExe, L"\\\\?\\" + win, Component::Yes, "extended-prefix Windows directory"},
        {cmdExe, win + L"\\", Component::Yes, "Windows directory with a trailing separator"},
        {cmdExe, L"", Component::Unknown, "Windows directory query failed (empty)"},
        {base + L"\\missing.exe", win, Component::Unknown, "image that cannot be opened"},
        {cmdExe, base + L"\\no-such-windows", Component::Unknown, "Windows directory that cannot be opened"},
    };

    // 8.3 alias of a long name below the Windows directory, when the volume has short names.
    const std::wstring longImage = win + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    wchar_t shortImage[MAX_PATH];
    const DWORD sn = GetShortPathNameW(longImage.c_str(), shortImage, MAX_PATH);
    if (sn && sn < MAX_PATH && _wcsicmp(shortImage, longImage.c_str()))
        cases.push_back({shortImage, win, Component::Yes, "8.3 alias image"});
    else
        printf("  note: no 8.3 alias for %ls (short names off on this volume); alias case skipped\n", longImage.c_str());

    // A junction outside the Windows directory that points into it.
    const std::wstring junction = base + L"\\winlink";
    RemoveDirectoryW(junction.c_str());
    const std::wstring mk = L"cmd /c mklink /J \"" + junction + L"\" \"" + win + L"\" >nul";
    if (_wsystem(mk.c_str()) == 0)
        cases.push_back({junction + L"\\System32\\cmd.exe", win, Component::Yes, "junction into the Windows directory"});
    else
        CHECK(false, "mklink /J %ls failed", junction.c_str());

    // Separator boundary on real directories: <base>\Windows is the "Windows directory" here.
    const wchar_t *dirs[] = {L"\\Windows", L"\\Windows2", L"\\Windows.old"};
    for (const wchar_t *d : dirs) {
        const std::wstring dir = base + d;
        CreateDirectoryW(dir.c_str(), nullptr);
        HANDLE f = CreateFileW((dir + L"\\game.exe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    cases.push_back({base + L"\\Windows\\game.exe", base + L"\\Windows", Component::Yes, "fixture Windows directory"});
    // 8.3 alias inside the fixture (the layout's volume may have short names where the system volume has not).
    const std::wstring longDir = base + L"\\Windows\\Long Directory Name";
    CreateDirectoryW(longDir.c_str(), nullptr);
    HANDLE lf = CreateFileW((longDir + L"\\game.exe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    wchar_t shortFixture[MAX_PATH];
    const DWORD fn = GetShortPathNameW((longDir + L"\\game.exe").c_str(), shortFixture, MAX_PATH);
    if (fn && fn < MAX_PATH && !wcsstr(shortFixture, L"Long Directory Name"))
        cases.push_back({shortFixture, base + L"\\Windows", Component::Yes, "8.3 alias in the fixture"});
    else
        printf("  note: no 8.3 alias on the layout volume either; fixture alias case skipped\n");
    cases.push_back({base + L"\\Windows2\\game.exe", base + L"\\Windows", Component::No, "Windows2 sibling"});
    cases.push_back({base + L"\\Windows.old\\game.exe", base + L"\\Windows", Component::No, "Windows.old sibling"});
    wchar_t self[MAX_PATH];
    const DWORD selfn = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (selfn && selfn < MAX_PATH)
        cases.push_back({self, win, Component::No, "this test's image (outside the Windows directory)"});

    for (const Case &c : cases) {
        const Component got = ClassifyComponent(c.image.c_str(), c.windows_dir.empty() ? nullptr : c.windows_dir.c_str());
        CHECK(got == c.want, "%s: ClassifyComponent(%ls, %ls) = %d, want %d", c.what, c.image.c_str(),
              c.windows_dir.c_str(), (int)got, (int)c.want);
    }
    CHECK(ClassifyComponent(nullptr, win.c_str()) == Component::Unknown, "null image is not Unknown");
    CHECK(ClassifyComponent(cmdExe.c_str(), nullptr) == Component::Unknown, "null Windows directory is not Unknown");
    // Removes the junction only, never its target. A failure would leave a link into the Windows directory behind.
    // Already absent counts only for the two not-found errors; any other state (query denied) fails.
    if (!RemoveDirectoryW(junction.c_str())) {
        const DWORD removeError = GetLastError();
        const DWORD attributes = GetFileAttributesW(junction.c_str());
        const DWORD queryError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        const bool absent = attributes == INVALID_FILE_ATTRIBUTES &&
                            (queryError == ERROR_FILE_NOT_FOUND || queryError == ERROR_PATH_NOT_FOUND);
        CHECK(absent, "junction %ls not removed: RemoveDirectoryW error %lu, then attributes 0x%lx error %lu",
              junction.c_str(), removeError, attributes, queryError);
    }
}

// ---------------------------------------------------------------- the M15.14 front (pure)

// The two table fills, driven with a hosted table of distinct recognizable values. A slot of the front's
// published table is a FIELD COPY exactly when it holds one of those values, and the front's OWN body
// otherwise, so the gate can say which entries the front forwards without calling a single one of them.
static const UINT_PTR HostedDeviceSlotBase = 0x00D10000;
static const UINT_PTR HostedDxgiSlotBase = 0x00D20000;

// The four table shapes this gate counts over. They are compile-time facts of the WDK headers, so they are
// asserted at compile time and not at run time: a WDK bump that changes one of them must fail the build, not
// one scenario. front-adapter.h asserts the same for the three tables the front itself publishes.
static_assert(sizeof(D3D10DDI_DEVICEFUNCS) == 101 * sizeof(void *), "D3D10DDI_DEVICEFUNCS is not 101 entries");
static_assert(sizeof(D3D11_1DDI_DEVICEFUNCS) == 155 * sizeof(void *), "D3D11_1DDI_DEVICEFUNCS is not 155 entries");
static_assert(sizeof(DXGI_DDI_BASE_FUNCTIONS) == 7 * sizeof(void *), "DXGI_DDI_BASE_FUNCTIONS is not 7 entries");
static_assert(sizeof(DXGI1_2_DDI_BASE_FUNCTIONS) == 15 * sizeof(void *), "DXGI1_2_DDI_BASE_FUNCTIONS is not 15 entries");

static void FrontTableTests()
{
    using namespace bc250front;
    D3D10DDI_DEVICEFUNCS hosted;
    void **hostedSlots = (void **)&hosted;
    const unsigned hostedCount = (unsigned)(sizeof(hosted) / sizeof(void *));
    for (unsigned i = 0; i < hostedCount; ++i) hostedSlots[i] = (void *)(HostedDeviceSlotBase + i);

    // Completeness assert 1: every one of the device entries the front publishes is non-null. A null slot is
    // a call into address zero inside dwm.exe, and the whole front stands or falls on this.
    D3D11_1DDI_DEVICEFUNCS front;
    memset(&front, 0, sizeof(front));
    FillDeviceFuncs(&front, hosted);
    void **frontSlots = (void **)&front;
    const unsigned frontCount = (unsigned)(sizeof(front) / sizeof(void *));
    printf("  D3D11_1DDI_DEVICEFUNCS: %u entries here, %u in the WDK declaration (the two D3D10PSGP members "
           "are reserved for system use and are not declared)\n",
           (unsigned)kDeviceEntries, (unsigned)kDeviceEntriesWithPsgp);
    unsigned nulls = 0, copies = 0, own = 0;
    for (unsigned i = 0; i < frontCount; ++i) {
        const UINT_PTR v = (UINT_PTR)frontSlots[i];
        if (!v) { ++nulls; printf("  device entry %u is null\n", i); }
        else if (v >= HostedDeviceSlotBase && v < HostedDeviceSlotBase + hostedCount) ++copies;
        else ++own;
    }
    CHECK(nulls == 0, "%u of the %u D3D11_1 device entries are null", nulls, frontCount);
    printf("  front device table: %u entries, %u field copies, %u own bodies\n", frontCount, copies, own);
    CHECK(copies + own == frontCount, "%u + %u != %u", copies, own, frontCount);
    // The exact split, so that a change to the forwarding table shows up here and is reviewed entry by entry.
    // 75 of the published entries hold a hosted function itself. The other 80 are the front's own body: 32
    // thunks and hooks (an argument struct that grew, a semantic the front has to carry, or the resource map
    // and the device record), 43 entries a 3D pipeline level 10_0 device cannot reach, and 5 the front
    // answers itself. The two tables below name all 155 of them.
    CHECK(copies == 75 && own == 80, "the table splits %u field copies and %u own bodies, expected 75 and 80",
          copies, own);

    auto isCopy = [&](const void *p) {
        const UINT_PTR v = (UINT_PTR)p;
        return v >= HostedDeviceSlotBase && v < HostedDeviceSlotBase + hostedCount;
    };
    // EVERY field copy, by name, with the hosted slot it must hold. A count alone cannot see a mis-wire
    // inside a family that shares a typedef: `out->pfnVsSetSamplers = hosted.pfnGsSetSamplers` compiles and
    // passes a count, and the compiler is the only guard for a CROSS-typedef error. A wrong stage binding is
    // the likeliest hand-transcription error in a 155-member table and it would show up on the lab as wrong
    // textures on the desktop, not as a crash. With this table the 75 is a checked mapping, not a total.
    struct Pair { size_t front_at, hosted_at; const char *name; };
#define FRONT_COPY(member) \
    { offsetof(D3D11_1DDI_DEVICEFUNCS, member), offsetof(D3D10DDI_DEVICEFUNCS, member), #member }
#define FRONT_COPY_AS(member, hostedMember)                                                  \
    { offsetof(D3D11_1DDI_DEVICEFUNCS, member), offsetof(D3D10DDI_DEVICEFUNCS, hostedMember), \
      #member " = hosted." #hostedMember }
    static const Pair copyPairs[] = {
        FRONT_COPY(pfnPsSetShaderResources),
        FRONT_COPY(pfnPsSetShader),
        FRONT_COPY(pfnPsSetSamplers),
        FRONT_COPY(pfnVsSetShader),
        FRONT_COPY(pfnDrawIndexed),
        FRONT_COPY(pfnDraw),
        FRONT_COPY(pfnDynamicIABufferMapNoOverwrite),
        FRONT_COPY(pfnDynamicIABufferUnmap),
        FRONT_COPY(pfnDynamicConstantBufferMapDiscard),
        FRONT_COPY(pfnDynamicIABufferMapDiscard),
        FRONT_COPY(pfnDynamicConstantBufferUnmap),
        FRONT_COPY(pfnIaSetInputLayout),
        FRONT_COPY(pfnIaSetVertexBuffers),
        FRONT_COPY(pfnIaSetIndexBuffer),
        FRONT_COPY(pfnDrawIndexedInstanced),
        FRONT_COPY(pfnDrawInstanced),
        FRONT_COPY(pfnDynamicResourceMapDiscard),
        FRONT_COPY(pfnDynamicResourceUnmap),
        FRONT_COPY(pfnGsSetShader),
        FRONT_COPY(pfnIaSetTopology),
        FRONT_COPY(pfnStagingResourceMap),
        FRONT_COPY(pfnStagingResourceUnmap),
        FRONT_COPY(pfnVsSetShaderResources),
        FRONT_COPY(pfnVsSetSamplers),
        FRONT_COPY(pfnGsSetShaderResources),
        FRONT_COPY(pfnGsSetSamplers),
        FRONT_COPY(pfnShaderResourceViewReadAfterWriteHazard),
        FRONT_COPY(pfnResourceReadAfterWriteHazard),
        FRONT_COPY(pfnSetBlendState),
        FRONT_COPY(pfnSetDepthStencilState),
        FRONT_COPY(pfnSetRasterizerState),
        FRONT_COPY(pfnQueryEnd),
        FRONT_COPY(pfnQueryBegin),
        FRONT_COPY(pfnSoSetTargets),
        FRONT_COPY(pfnDrawAuto),
        FRONT_COPY(pfnSetViewports),
        FRONT_COPY(pfnSetScissorRects),
        FRONT_COPY(pfnClearRenderTargetView),
        FRONT_COPY(pfnClearDepthStencilView),
        FRONT_COPY(pfnSetPredication),
        FRONT_COPY(pfnQueryGetData),
        FRONT_COPY(pfnGenMips),
        FRONT_COPY(pfnResourceCopy),
        FRONT_COPY(pfnResourceResolveSubresource),
        FRONT_COPY(pfnResourceMap),
        FRONT_COPY(pfnResourceUnmap),
        FRONT_COPY(pfnResourceIsStagingBusy),
        FRONT_COPY(pfnCalcPrivateOpenedResourceSize),
        FRONT_COPY(pfnDestroyShaderResourceView),
        FRONT_COPY(pfnCalcPrivateRenderTargetViewSize),
        FRONT_COPY(pfnCreateRenderTargetView),
        FRONT_COPY(pfnDestroyRenderTargetView),
        FRONT_COPY(pfnDestroyDepthStencilView),
        FRONT_COPY(pfnCalcPrivateElementLayoutSize),
        FRONT_COPY(pfnCreateElementLayout),
        FRONT_COPY(pfnDestroyElementLayout),
        FRONT_COPY(pfnDestroyBlendState),
        FRONT_COPY(pfnCalcPrivateDepthStencilStateSize),
        FRONT_COPY(pfnCreateDepthStencilState),
        FRONT_COPY(pfnDestroyDepthStencilState),
        FRONT_COPY(pfnDestroyRasterizerState),
        FRONT_COPY(pfnDestroyShader),
        FRONT_COPY(pfnCalcPrivateSamplerSize),
        FRONT_COPY(pfnCreateSampler),
        FRONT_COPY(pfnDestroySampler),
        FRONT_COPY(pfnCalcPrivateQuerySize),
        FRONT_COPY(pfnCreateQuery),
        FRONT_COPY(pfnDestroyQuery),
        FRONT_COPY(pfnCheckFormatSupport),
        FRONT_COPY(pfnCheckMultisampleQualityLevels),
        FRONT_COPY(pfnCheckCounterInfo),
        FRONT_COPY(pfnCheckCounter),
        FRONT_COPY(pfnSetTextFilterSize),
        FRONT_COPY_AS(pfnResourceConvert, pfnResourceCopy),
        FRONT_COPY_AS(pfnDynamicConstantBufferMapNoOverwrite, pfnResourceMap),
    };
#undef FRONT_COPY
#undef FRONT_COPY_AS
    // The two deliberate cross-name forwards are in that table by name, so a reviewer reads them there:
    // pfnResourceConvert takes the hosted pfnResourceCopy (the 10.1 convert is a copy at this DDI) and
    // pfnDynamicConstantBufferMapNoOverwrite takes the generic hosted pfnResourceMap.
    static_assert(ARRAYSIZE(copyPairs) == 75, "the copy table must name all 75 field copies");
    std::vector<char> covered(frontCount, 0);
    for (const Pair &p : copyPairs) {
        const void *got = *(void **)((BYTE *)&front + p.front_at);
        const void *want = (const void *)(HostedDeviceSlotBase + p.hosted_at / sizeof(void *));
        CHECK(got == want, "%s is not the hosted entry it names (slot %Iu, got slot %Id)", p.name,
              p.hosted_at / sizeof(void *),
              (ptrdiff_t)((UINT_PTR)got - HostedDeviceSlotBase) / (ptrdiff_t)sizeof(void *));
        covered[p.front_at / sizeof(void *)] = 1;
    }
    // And no field copy outside that table: a new copy added to FillDeviceFuncs without a line here is a
    // forward nobody reviewed.
    for (unsigned i = 0; i < frontCount; ++i)
        CHECK(!isCopy(frontSlots[i]) || covered[i], "device entry %u is a field copy the gate does not name",
              i);

    // And every own body that is NOT a refusal, by name: the thunks, the four resource and device hooks and
    // the five answers the front gives itself. 37 names; the remaining 43 of the 80 own bodies are the
    // refusals, which is how the split below is checked rather than asserted.
    struct Own { size_t front_at; const char *name; };
#define FRONT_OWN(member) { offsetof(D3D11_1DDI_DEVICEFUNCS, member), #member }
    static const Own ownBodies[] = {
        FRONT_OWN(pfnDefaultConstantBufferUpdateSubresourceUP),
        FRONT_OWN(pfnVsSetConstantBuffers),
        FRONT_OWN(pfnPsSetConstantBuffers),
        FRONT_OWN(pfnGsSetConstantBuffers),
        FRONT_OWN(pfnSetRenderTargets),
        FRONT_OWN(pfnResourceCopyRegion),
        FRONT_OWN(pfnResourceUpdateSubresourceUP),
        FRONT_OWN(pfnFlush),
        FRONT_OWN(pfnRelocateDeviceFuncs),
        FRONT_OWN(pfnCalcPrivateResourceSize),
        FRONT_OWN(pfnCreateResource),
        FRONT_OWN(pfnOpenResource),
        FRONT_OWN(pfnDestroyResource),
        FRONT_OWN(pfnCalcPrivateShaderResourceViewSize),
        FRONT_OWN(pfnCreateShaderResourceView),
        FRONT_OWN(pfnCalcPrivateDepthStencilViewSize),
        FRONT_OWN(pfnCreateDepthStencilView),
        FRONT_OWN(pfnCalcPrivateBlendStateSize),
        FRONT_OWN(pfnCreateBlendState),
        FRONT_OWN(pfnCalcPrivateRasterizerStateSize),
        FRONT_OWN(pfnCreateRasterizerState),
        FRONT_OWN(pfnCalcPrivateShaderSize),
        FRONT_OWN(pfnCreateVertexShader),
        FRONT_OWN(pfnCreateGeometryShader),
        FRONT_OWN(pfnCreatePixelShader),
        FRONT_OWN(pfnCalcPrivateGeometryShaderWithStreamOutput),
        FRONT_OWN(pfnCreateGeometryShaderWithStreamOutput),
        FRONT_OWN(pfnDestroyDevice),
        FRONT_OWN(pfnResourceConvertRegion),
        FRONT_OWN(pfnCheckDeferredContextHandleSizes),
        FRONT_OWN(pfnPsSetShaderWithIfaces),
        FRONT_OWN(pfnVsSetShaderWithIfaces),
        FRONT_OWN(pfnGsSetShaderWithIfaces),
        FRONT_OWN(pfnDiscard),
        FRONT_OWN(pfnAssignDebugBinary),
        FRONT_OWN(pfnCheckDirectFlipSupport),
        FRONT_OWN(pfnClearView),
    };
#undef FRONT_OWN
    static_assert(ARRAYSIZE(ownBodies) == 37, "the own-body table must name all 37 non-refusal own bodies");
    // 75 + 37 + 43 = 155. The 43 refusals are the remainder, so this one line is what keeps the split in
    // front-device.cpp's header comment honest.
    static_assert(kDeviceEntries == ARRAYSIZE(copyPairs) + ARRAYSIZE(ownBodies) + 43,
                  "the three groups no longer add up to the published table");
    for (const Own &o : ownBodies) {
        const void *got = *(void **)((BYTE *)&front + o.front_at);
        CHECK(got != nullptr, "%s is null", o.name);
        CHECK(!isCopy(got), "%s is a field copy, but the front owes it a body of its own", o.name);
    }
    const unsigned refusals = frontCount - (unsigned)ARRAYSIZE(copyPairs) - (unsigned)ARRAYSIZE(ownBodies);
    printf("  front device table split: %u field copies, %u thunks hooks and own answers, %u refusals\n",
           (unsigned)ARRAYSIZE(copyPairs), (unsigned)ARRAYSIZE(ownBodies), refusals);

    // Completeness assert 2: the DXGI table. The hosted driver fills 7 of the 15 through the union member and
    // the front owes the rest; at build version 0 the last four are excluded by the operating system and
    // filled all the same, so that no slot is null whatever the assumption turns out to be.
    DXGI_DDI_BASE_FUNCTIONS hostedDxgi;
    void **hostedDxgiSlots = (void **)&hostedDxgi;
    const unsigned hostedDxgiCount = (unsigned)(sizeof(hostedDxgi) / sizeof(void *));
    for (unsigned i = 0; i < hostedDxgiCount; ++i) hostedDxgiSlots[i] = (void *)(HostedDxgiSlotBase + i);
    DXGI1_2_DDI_BASE_FUNCTIONS frontDxgi;
    memset(&frontDxgi, 0, sizeof(frontDxgi));
    FillDxgiFuncs(&frontDxgi, hostedDxgi);
    void **frontDxgiSlots = (void **)&frontDxgi;
    const unsigned frontDxgiCount = (unsigned)(sizeof(frontDxgi) / sizeof(void *));
    unsigned dxgiNulls = 0, dxgiCopies = 0, dxgiOwn = 0;
    for (unsigned i = 0; i < frontDxgiCount; ++i) {
        const UINT_PTR v = (UINT_PTR)frontDxgiSlots[i];
        if (!v) { ++dxgiNulls; printf("  DXGI entry %u is null\n", i); }
        else if (v >= HostedDxgiSlotBase && v < HostedDxgiSlotBase + hostedDxgiCount) {
            ++dxgiCopies;
            CHECK(i < hostedDxgiCount && v == HostedDxgiSlotBase + i, "DXGI entry %u copies the wrong slot", i);
        } else {
            ++dxgiOwn;
            CHECK(i >= hostedDxgiCount, "DXGI entry %u should be the hosted entry itself", i);
        }
    }
    CHECK(dxgiNulls == 0, "%u of the %u DXGI1_2 entries are null", dxgiNulls, frontDxgiCount);
    CHECK(dxgiCopies == 7 && dxgiOwn == 8, "DXGI: %u field copies and %u own bodies, expected 7 and 8",
          dxgiCopies, dxgiOwn);
    printf("  front DXGI table: %u entries, %u field copies, %u own bodies\n", frontDxgiCount, dxgiCopies, dxgiOwn);
    // Slot 7, pfnResolveSharedResource, arrived at DXGI1_1. Whether the hosted driver filled it depends on
    // IS_DXGI1_1_BASE_FUNCTIONS over the runtime's own `Version`, which the front does not control, so the
    // front keeps a non-null slot and installs its stub only in an empty one. The zeroed case above is the
    // stub; this is the other one.
    DXGI1_2_DDI_BASE_FUNCTIONS keptDxgi;
    memset(&keptDxgi, 0, sizeof(keptDxgi));
    void *const hostedResolve = (void *)(UINT_PTR)0x00D2BEEF;
    *((void **)&keptDxgi + 7) = hostedResolve;
    FillDxgiFuncs(&keptDxgi, hostedDxgi);
    CHECK((void *)keptDxgi.pfnResolveSharedResource == hostedResolve,
          "the front overwrote the hosted driver's own pfnResolveSharedResource");
    CHECK((void *)frontDxgi.pfnResolveSharedResource != hostedResolve &&
          frontDxgi.pfnResolveSharedResource != nullptr,
          "an empty pfnResolveSharedResource slot did not get the front's stub");
    unsigned keptNulls = 0;
    for (unsigned i = 0; i < frontDxgiCount; ++i) if (!*((void **)&keptDxgi + i)) ++keptNulls;
    CHECK(keptNulls == 0, "%u DXGI entries are null when slot 7 came from the hosted driver", keptNulls);
}

// ---------------------------------------------------------------- the DirectFlip rule (pure)

// A record the two scannable derivations accept, built the way the contract says each side earns it.
static BC250_SURFACE_RESOURCE_PRIVATE FlipRecord(bool shared, unsigned long access)
{
    BC250_SURFACE_RESOURCE_PRIVATE r;
    memset(&r, 0, sizeof(r));
    r.Magic = BC250_SURFACE_RESOURCE_MAGIC;
    r.Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    r.Shared = shared ? 1ul : 0ul;
    r.Access = access;
    return r;
}

static void FrontRuleTests()
{
    using namespace bc250front;
    bc250_scanout_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.magic = BC250_SCANOUT_CAPS_MAGIC;
    caps.version = BC250_SCANOUT_CAPS_VERSION;
    caps.flags = BC250_SCANOUT_CAPS_DIRECT_FLIP;
    caps.post_width = 1920;
    caps.post_height = 1080;

    // The one pair that passes every clause: the application's buffer opened into the compositor's device
    // with its own SCANOUT bit, and the compositor's own front buffer, same geometry, same pitch, the POST
    // mode's geometry, one video present source, a scan-out storage row.
    Resource client;
    memset(&client, 0, sizeof(client));
    client.recorded = true;
    client.opened = true;
    client.primary = true;
    client.shared = true;
    client.vidpn_source = BC250_SCANOUT_VIDPN_SOURCE;
    client.width = 1920;
    client.height = 1080;
    client.pitch = 1920 * 4;
    client.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    client.record = FlipRecord(true, BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT);
    Resource compositor = client;
    compositor.opened = false;
    compositor.shared = false;
    compositor.record = FlipRecord(false, BC250_SURFACE_RESOURCE_PRIMARY);
    CHECK(FlipReason(caps, &client, &compositor) == FlipRefusal::none, "the admissible pair is refused: %s",
          FlipRefusalText(FlipReason(caps, &client, &compositor)));
    CHECK(FlipSupported(caps, &client, &compositor), "FlipSupported disagrees with FlipReason");

    // One refusal per clause, in the order the rule applies them. Each case changes exactly one thing.
    auto run = [&](const bc250_scanout_caps &c, const Resource *a, const Resource *b, FlipRefusal want,
                   const char *what) {
        const FlipRefusal got = FlipReason(c, a, b);
        CHECK(got == want, "%s: rule says %s, expected %s", what, FlipRefusalText(got), FlipRefusalText(want));
    };
    run(caps, nullptr, &compositor, FlipRefusal::handle, "a null client handle");
    run(caps, &client, nullptr, FlipRefusal::handle, "a null compositor handle");
    run(caps, &client, &client, FlipRefusal::handle, "one surface given twice");
    {
        bc250_scanout_caps c = caps;
        c.magic = 0;
        run(c, &client, &compositor, FlipRefusal::gated, "no caps trailer");
        c = caps;
        c.version = BC250_SCANOUT_CAPS_VERSION + 1;
        run(c, &client, &compositor, FlipRefusal::gated, "a caps trailer of another version");
        c = caps;
        c.flags = 0;
        run(c, &client, &compositor, FlipRefusal::gated, "the kernel driver published no DirectFlip");
        c = caps;
        c.post_width = 1280;
        run(c, &client, &compositor, FlipRefusal::source_geometry, "a source mode of another width");
        c = caps;
        c.post_height = 720;
        run(c, &client, &compositor, FlipRefusal::source_geometry, "a source mode of another height");
        // No mode is built into the rule: the same pair at 1280x720 passes against a trailer that says
        // 1280x720, and the 1920x1200 POST mode of this lab is only one value of the trailer.
        Resource lowRes = client, lowResCompositor = compositor;
        lowRes.width = lowResCompositor.width = 1280;
        lowRes.height = lowResCompositor.height = 720;
        lowRes.pitch = lowResCompositor.pitch = HostedSurfacePitch(1280, 4);
        c = caps;
        c.post_width = 1280;
        c.post_height = 720;
        run(c, &lowRes, &lowResCompositor, FlipRefusal::none, "a 1280x720 pair against a 1280x720 source mode");
        run(caps, &lowRes, &lowResCompositor, FlipRefusal::source_geometry,
            "a 1280x720 pair against a 1920x1080 source mode");
    }
    {
        Resource a = client;
        a.recorded = false;
        run(caps, &a, &compositor, FlipRefusal::record, "a client the front never recorded");
        a = client;
        a.opened = false;
        run(caps, &a, &compositor, FlipRefusal::sides, "a client this device created itself");
        a = client;
        a.primary = false;
        run(caps, &a, &compositor, FlipRefusal::primary, "a client that is not a primary");
        a = client;
        a.vidpn_source = 1;
        run(caps, &a, &compositor, FlipRefusal::vidpn_source, "a client on another video present source");
        a = client;
        a.record = FlipRecord(true, BC250_SURFACE_RESOURCE_PRIMARY);
        run(caps, &a, &compositor, FlipRefusal::client_scannable, "a client record with no SCANOUT bit");
        a = client;
        memset(&a.record, 0, sizeof(a.record));
        run(caps, &a, &compositor, FlipRefusal::client_scannable, "a client record that cannot be read");
        a = client;
        a.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        run(caps, &a, &compositor, FlipRefusal::format, "a storage row that is not a scan-out row");
        // M15.14 (0.7.216.20): RGBA8 and RGB10A2 are scan-out rows, but a row that is not the firmware's
        // own format needs the trailer's PLANE_FORMATS flag, because only that kernel driver programs the
        // plane's pixel format. With the flag, the client's row may differ from the compositor's BGRA8
        // row (the swizzle and the address latch in one VUPDATE); FP16 stays refused.
        bc250_scanout_caps planes = caps;
        planes.flags |= BC250_SCANOUT_CAPS_PLANE_FORMATS;
        const DXGI_FORMAT newRows[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM};
        for (DXGI_FORMAT format : newRows) {
            a = client;
            a.format = format;
            run(caps, &a, &compositor, FlipRefusal::format, "a new scan-out row without PLANE_FORMATS");
            run(planes, &a, &compositor, FlipRefusal::none, "a new scan-out row with PLANE_FORMATS");
            Resource b = compositor;
            b.format = format;
            run(caps, &a, &b, FlipRefusal::format, "two new rows without PLANE_FORMATS");
            run(planes, &a, &b, FlipRefusal::none, "two new rows with PLANE_FORMATS");
            run(planes, &client, &b, FlipRefusal::none, "a BGRA8 client over a new compositor row");
        }
        a = client;
        run(planes, &a, &compositor, FlipRefusal::none, "BGRA8 with PLANE_FORMATS");
        a.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        a.pitch = HostedSurfacePitch(1920, 8);
        run(planes, &a, &compositor, FlipRefusal::format, "FP16 with PLANE_FORMATS");
        a = client;
        a.format = DXGI_FORMAT_A8_UNORM;
        run(planes, &a, &compositor, FlipRefusal::format, "A8 with PLANE_FORMATS");
        a = client;
        a.format = 0;
        run(planes, &a, &compositor, FlipRefusal::format, "a format the record and the blob disagree on");
        {
            Resource b = compositor;
            b.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            run(planes, &client, &b, FlipRefusal::format, "an FP16 compositor buffer (an HDR desktop)");
        }
        a = client;
        a.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        a.pitch = 1920 * 4 + 256;
        run(planes, &a, &compositor, FlipRefusal::pitch, "an RGBA8 client of another pitch");
        a = client;
        a.width = 1280;
        run(caps, &a, &compositor, FlipRefusal::geometry, "a client of another width");
        a = client;
        a.height = 720;
        run(caps, &a, &compositor, FlipRefusal::geometry, "a client of another height");
        a = client;
        a.pitch = 0;
        run(caps, &a, &compositor, FlipRefusal::pitch_unknown, "a client pitch that is not known here");
        a = client;
        a.pitch = 1920 * 4 + 256;
        run(caps, &a, &compositor, FlipRefusal::pitch, "two different pitches");
    }
    {
        Resource b = compositor;
        b.recorded = false;
        run(caps, &client, &b, FlipRefusal::record, "a compositor surface the front never recorded");
        b = compositor;
        b.opened = true;
        run(caps, &client, &b, FlipRefusal::sides, "a compositor surface that was opened, not created");
        b = compositor;
        b.primary = false;
        run(caps, &client, &b, FlipRefusal::primary, "a compositor surface that is not a primary");
        b = compositor;
        memset(&b.record, 0, sizeof(b.record));
        run(caps, &client, &b, FlipRefusal::compositor_scannable, "a compositor record that cannot be read");
        b = compositor;
        b.pitch = 0;
        run(caps, &client, &b, FlipRefusal::pitch_unknown, "a compositor pitch that is not known here");
        // The asymmetry itself: the compositor's own buffer never asks for scan-out, and demanding the bit of
        // it would refuse every pair the operating system can pass. Its record with the bit set is admitted
        // too, because WddmGdiCreatedScannable does not read the bit at all.
        b = compositor;
        b.record = FlipRecord(false, BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT);
        run(caps, &client, &b, FlipRefusal::none, "a compositor record that also asks for scan-out");
    }
    // The pair the other way round is a fail-safe FALSE and never a flip of the wrong buffer.
    run(caps, &compositor, &client, FlipRefusal::sides, "the pair given the other way round");
    // Every refusal has a name, including one the enum does not define.
    CHECK(!strcmp(FlipRefusalText(FlipRefusal::none), "supported"), "FlipRefusalText(none)");
    CHECK(!strcmp(FlipRefusalText((FlipRefusal)99), "unknown"), "FlipRefusalText of an unknown value");
    CHECK(!strcmp(FlipRefusalText(FlipRefusal::source_geometry), "source-geometry"),
          "FlipRefusalText(source_geometry)");

    // The compositor's pitch: the hosted driver's own line (the row in bytes rounded up to 256), and the
    // kernel driver's primary pitch for a 4-byte row. 1920 and 1280 are already whole multiples; 1366 is not.
    CHECK(HostedSurfacePitch(1920, 4) == 7680, "HostedSurfacePitch(1920, 4) = %u", HostedSurfacePitch(1920, 4));
    CHECK(HostedSurfacePitch(1280, 4) == 5120, "HostedSurfacePitch(1280, 4) = %u", HostedSurfacePitch(1280, 4));
    CHECK(HostedSurfacePitch(1366, 4) == 5632, "HostedSurfacePitch(1366, 4) = %u", HostedSurfacePitch(1366, 4));
    CHECK(HostedSurfacePitch(1, 4) == 256, "HostedSurfacePitch(1, 4) = %u", HostedSurfacePitch(1, 4));
    CHECK(HostedSurfacePitch(1920, 8) == 15360, "HostedSurfacePitch(1920, 8) = %u", HostedSurfacePitch(1920, 8));
    CHECK(HostedSurfacePitch(0, 4) == 0 && HostedSurfacePitch(1920, 0) == 0 &&
          HostedSurfacePitch(0x7FFFFFFFu, 4) == 0, "HostedSurfacePitch of no row is not 0");

    // The trailer decode. The negative controls first: each one is a reader that must see zeros.
    unsigned char buffer[BC250_SCANOUT_CAPS_TOTAL];
    bc250_scanout_caps read;
    const bc250_scanout_caps good = {BC250_SCANOUT_CAPS_MAGIC, BC250_SCANOUT_CAPS_VERSION, sizeof(good),
                                     BC250_SCANOUT_CAPS_DIRECT_FLIP, 2560, 1440};
    auto decoded = [&](const bc250_scanout_caps &trailer, size_t bytes) {
        memset(buffer, 0, sizeof(buffer));
        memcpy(buffer + BC250_SCANOUT_CAPS_OFFSET, &trailer, sizeof(trailer));
        memset(&read, 0xA5, sizeof(read));
        DecodeScanoutCaps(buffer, bytes, &read);
        return read;
    };
    auto zero = [](const bc250_scanout_caps &c) {
        return !c.magic && !c.version && !c.size && !c.flags && !c.post_width && !c.post_height;
    };
    CHECK(decoded(good, sizeof(buffer)).post_width == 2560 && read.post_height == 1440 &&
          read.flags == BC250_SCANOUT_CAPS_DIRECT_FLIP, "a whole trailer did not decode");
    CHECK(zero(decoded(good, sizeof(buffer) - 1)), "a buffer one byte short of the trailer decoded");
    bc250_scanout_caps torn = good;
    torn.magic = 0;
    CHECK(zero(decoded(torn, sizeof(buffer))), "a trailer with no magic decoded");
    torn = good;
    torn.version = BC250_SCANOUT_CAPS_VERSION + 1;
    CHECK(zero(decoded(torn, sizeof(buffer))), "a trailer of another version decoded");
    torn = good;
    torn.size = sizeof(torn) + 4;
    CHECK(zero(decoded(torn, sizeof(buffer))), "a trailer of another size decoded");
    torn = good;
    torn.post_height = 0;
    CHECK(zero(decoded(torn, sizeof(buffer))), "a trailer with no geometry decoded");
    memset(&read, 0xA5, sizeof(read));
    DecodeScanoutCaps(nullptr, sizeof(buffer), &read);
    CHECK(zero(read), "a null buffer decoded");
    // And the read through the adapter: no adapter, or one without a callback, is E_POINTER and zero.
    CHECK(ReadScanoutCaps(nullptr, &read) == E_POINTER && zero(read), "a read with no adapter");
}

// ---------------------------------------------------------------- the records and the resource map (pure)

static void FrontRecordTests()
{
    using namespace bc250front;
    BC250_SURFACE_RESOURCE_PRIVATE out;
    // E26R v1: 12 bytes, three words, no access word at all. It is normalized into a v3-shaped struct and
    // keeps the version it had, so that the DirectFlip rule cannot be fooled by a normalized v1 record.
    unsigned long v1[3] = {BC250_SURFACE_RESOURCE_MAGIC, 1ul, 1ul};
    CHECK(DecodeRecord(v1, sizeof(v1), &out), "a 12-byte v1 record was refused");
    CHECK(out.Magic == BC250_SURFACE_RESOURCE_MAGIC && out.Version == 1 && out.Shared == 1 && out.Access == 0,
          "v1 decode: version=%lu shared=%lu access=%lu", out.Version, out.Shared, out.Access);
    CHECK(!WddmGdiRecordScannable(&out, sizeof(out)), "a normalized v1 record must not be scannable");
    CHECK(!WddmGdiCreatedScannable(&out, sizeof(out)), "a normalized v1 record must not be scannable");
    // E26R v2: 16 bytes, with the access word.
    unsigned long v2[4] = {BC250_SURFACE_RESOURCE_MAGIC, 2ul, 1ul, BC250_SURFACE_RESOURCE_PRIMARY};
    CHECK(DecodeRecord(v2, sizeof(v2), &out), "a 16-byte v2 record was refused");
    CHECK(out.Version == 2 && out.Access == BC250_SURFACE_RESOURCE_PRIMARY, "v2 decode: version=%lu access=%lu",
          out.Version, out.Access);
    CHECK(!WddmGdiRecordScannable(&out, sizeof(out)), "a normalized v2 record must not be scannable");
    // E26R v3: 64 bytes, the whole texture description, copied through unchanged.
    BC250_SURFACE_RESOURCE_PRIVATE v3 = FlipRecord(true, BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT);
    v3.Width = 1920;
    v3.Height = 1080;
    v3.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    CHECK(DecodeRecord(&v3, sizeof(v3), &out), "a 64-byte v3 record was refused");
    CHECK(!memcmp(&out, &v3, sizeof(out)), "a v3 record was not copied through unchanged");
    CHECK(WddmGdiRecordScannable(&out, sizeof(out)), "a v3 SCANOUT record is not scannable");
    // The refusals. Each one leaves a zeroed record, which both derivations refuse.
    struct Bad { const void *data; unsigned int bytes; const char *what; };
    unsigned long noMagic[3] = {0x12345678ul, 3ul, 0ul};
    unsigned long badAccess[4] = {BC250_SURFACE_RESOURCE_MAGIC, 2ul, 1ul, 0x40ul};
    unsigned long scanoutNoPrimary[4] = {BC250_SURFACE_RESOURCE_MAGIC, 2ul, 1ul, BC250_SURFACE_RESOURCE_SCANOUT};
    const Bad bad[] = {
        {nullptr, 0, "a null blob"},
        {v1, 4, "four bytes"},
        {v1, 8, "eight bytes"},
        {noMagic, sizeof(noMagic), "a blob with no E26R magic"},
        {v2, 20, "a v2 record of the wrong length"},
        {&v3, 60, "a v3 record of the wrong length"},
        {badAccess, sizeof(badAccess), "an access word with an undefined bit"},
        {scanoutNoPrimary, sizeof(scanoutNoPrimary), "SCANOUT without PRIMARY"},
    };
    for (const Bad &b : bad) {
        memset(&out, 0xCC, sizeof(out));
        const bool got = DecodeRecord(b.data, b.bytes, &out);
        CHECK(!got, "%s was admitted as a record", b.what);
        BC250_SURFACE_RESOURCE_PRIVATE zero;
        memset(&zero, 0, sizeof(zero));
        CHECK(!memcmp(&out, &zero, sizeof(out)), "%s left a record behind", b.what);
    }

    // The LB7A allocation blob: 32 bytes, and the same 32-byte prefix inside a 48-byte GDI1 blob.
    unsigned int w = 0, h = 0, pitch = 0, format = 0;
    BC250_WDDM_ALLOCATION_PRIVATE lb7a;
    memset(&lb7a, 0, sizeof(lb7a));
    lb7a.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    lb7a.Version = 1;
    lb7a.Width = 1920;
    lb7a.Height = 1080;
    lb7a.Pitch = 1920 * 4;
    lb7a.Format = 21; // D3DDDIFMT_A8R8G8B8
    lb7a.Size = (unsigned long long)lb7a.Pitch * lb7a.Height;
    CHECK(DecodeAllocation(&lb7a, sizeof(lb7a), &w, &h, &pitch, &format) && w == 1920 && h == 1080 &&
          pitch == 1920 * 4 && format == 21, "LB7A v1 decode: %ux%u pitch=%u format=%u", w, h, pitch, format);
    BC250_GDI_PRIVATE gdi;
    memset(&gdi, 0, sizeof(gdi));
    gdi.Surface = lb7a;
    gdi.Magic = BC250_GDI_PRIVATE_MAGIC;
    gdi.Type = 1;
    CHECK(DecodeAllocation(&gdi, sizeof(gdi), &w, &h, &pitch, &format) && w == 1920 && pitch == 1920 * 4,
          "a 48-byte GDI1 blob carries the same 32-byte prefix");
    lb7a.Magic = 0;
    CHECK(!DecodeAllocation(&lb7a, sizeof(lb7a), &w, &h, &pitch, &format), "a blob with no LB7A magic");
    CHECK(!DecodeAllocation(&lb7a, 16, &w, &h, &pitch, &format), "a blob shorter than LB7A v1");
    CHECK(!DecodeAllocation(nullptr, 32, &w, &h, &pitch, &format), "a null allocation blob");

    // The resource map: record, find a copy, forget, and a full table that refuses further surfaces instead
    // of growing. The handles are the shapes a private block has: aligned, far apart, and reused.
    ResetResources();
    CHECK(RecordedResources() == 0, "the map is not empty after ResetResources");
    Resource r;
    memset(&r, 0, sizeof(r));
    r.recorded = true;
    r.width = 1920;
    r.height = 1080;
    void *h1 = (void *)(UINT_PTR)0x20000000;
    void *h2 = (void *)(UINT_PTR)0x20000100;
    CHECK(RecordResource(nullptr, h1, r), "a first surface was not recorded");
    r.width = 1280;
    CHECK(RecordResource(nullptr, h2, r), "a second surface was not recorded");
    CHECK(RecordedResources() == 2, "the map holds %u entries, expected 2", RecordedResources());
    Resource got;
    CHECK(FindResource(h1, &got) && got.width == 1920, "the first surface came back wrong");
    CHECK(FindResource(h2, &got) && got.width == 1280, "the second surface came back wrong");
    CHECK(!FindResource((void *)(UINT_PTR)0x20000200, &got), "an unknown handle was found");
    CHECK(!FindResource(nullptr, &got), "a null handle was found");
    // A handle the runtime reused: the new record replaces the old one and the count does not grow.
    r.width = 640;
    CHECK(RecordResource(nullptr, h1, r) && RecordedResources() == 2, "a reused handle grew the map");
    CHECK(FindResource(h1, &got) && got.width == 640, "a reused handle kept the old record");
    // A deletion must not break the probe chain of an entry behind it: h1 and h3 share a slot by design.
    void *h3 = (void *)(UINT_PTR)(0x20000000 + (UINT_PTR)kResourceSlots * 16);
    CHECK(RecordResource(nullptr, h3, r), "a colliding surface was not recorded");
    ForgetResource(h1);
    CHECK(!FindResource(h1, &got), "a forgotten surface is still found");
    CHECK(FindResource(h3, &got), "a deletion broke the probe chain behind it");
    CHECK(RecordedResources() == 2, "the map holds %u entries after one deletion, expected 2", RecordedResources());
    // A full table. Every slot is taken and the next surface is refused, not dropped silently: the rule then
    // answers FALSE for it under the name "record".
    ResetResources();
    unsigned recorded = 0;
    for (unsigned i = 0; i < kResourceSlots + 8; ++i)
        if (RecordResource(nullptr, (void *)(UINT_PTR)(0x30000000 + (UINT_PTR)i * 16), r)) ++recorded;
    CHECK(recorded == kResourceSlots, "a full map took %u surfaces, expected %u", recorded,
          (unsigned)kResourceSlots);
    CHECK(RecordedResources() == kResourceSlots, "the map reports %u of %u", RecordedResources(),
          (unsigned)kResourceSlots);
    ResetResources();
    CHECK(RecordedResources() == 0, "the map is not empty at the end");
}

// ---------------------------------------------------------------- scenarios

// ---------------------------------------------------------------- M15.14 increment 2: the answer, end to end

// The front over the double, with surfaces the front recorded through its own CreateResource and
// OpenResource hooks, and the kernel driver's scan-out caps trailer in the adapter query. The positive case
// is the pair the operating system passes on the lab: hResource1 the application's swap-chain buffer opened
// into the compositor's device (LB7A blob plus an E26R v3 record with PRIMARY and SCANOUT, as the D3D12 shell
// writes it), hResource2 the compositor's own primary (a create with a primary descriptor). Each negative
// control changes one input and must turn the answer FALSE under the clause it names.
static void FrontAnswerChecks(const D3D11_1DDI_DEVICEFUNCS &device, D3D10DDI_HDEVICE hDevice,
                              const char *(*record)(void), void (*recordReset)(void))
{
    using namespace bc250front;
    const unsigned width = 1920, height = 1200;
    // The compositor's own front buffer.
    D3D10DDI_MIPINFO mip = {};
    mip.TexelWidth = width;
    mip.TexelHeight = height;
    mip.TexelDepth = 1;
    mip.PhysicalWidth = width;
    mip.PhysicalHeight = height;
    mip.PhysicalDepth = 1;
    DXGI_DDI_PRIMARY_DESC primary = {};
    primary.VidPnSourceId = 0;
    primary.ModeDesc.Width = width;
    primary.ModeDesc.Height = height;
    primary.ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    D3D11DDIARG_CREATERESOURCE create = {};
    create.pMipInfoList = &mip;
    create.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
    create.Usage = D3D10_DDI_USAGE_DEFAULT;
    create.BindFlags = D3D10_DDI_BIND_RENDER_TARGET | D3D10_DDI_BIND_PRESENT;
    create.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    create.SampleDesc.Count = 1;
    create.MipLevels = 1;
    create.ArraySize = 1;
    create.pPrimaryDesc = &primary;
    D3D10DDI_HRESOURCE compositor, client, other;
    compositor.pDrvPrivate = (void *)(UINT_PTR)0x41000000;
    client.pDrvPrivate = (void *)(UINT_PTR)0x41000100;
    other.pDrvPrivate = (void *)(UINT_PTR)0x41000200;
    D3D10DDI_HRTRESOURCE rt = {};
    recordReset();
    device.pfnCreateResource(hDevice, &create, compositor, rt);
    CHECK(strstr(record(), "create-resource") != nullptr, "CreateResource did not reach the hosted driver: %s",
          record());
    // The application's buffer, opened: the LB7A blob of its allocation and its E26R v3 resource record.
    BC250_WDDM_ALLOCATION_PRIVATE lb7a = {};
    lb7a.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    lb7a.Version = 1;
    lb7a.Width = width;
    lb7a.Height = height;
    lb7a.Pitch = HostedSurfacePitch(width, 4);
    lb7a.Format = D3DDDIFMT_A8R8G8B8;
    lb7a.Size = (unsigned long long)lb7a.Pitch * height;
    BC250_SURFACE_RESOURCE_PRIVATE e26r = {};
    e26r.Magic = BC250_SURFACE_RESOURCE_MAGIC;
    e26r.Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    e26r.Shared = 1;
    e26r.Access = BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT;
    e26r.Width = width;
    e26r.Height = height;
    e26r.MipLevels = 1;
    e26r.ArraySize = 1;
    e26r.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    e26r.SampleCount = 1;
    D3DDDI_OPENALLOCATIONINFO allocation = {};
    allocation.hAllocation = 0x1234;
    allocation.pPrivateDriverData = &lb7a;
    allocation.PrivateDriverDataSize = sizeof(lb7a);
    D3D10DDIARG_OPENRESOURCE open = {};
    open.NumAllocations = 1;
    open.pOpenAllocationInfo = &allocation;
    open.pPrivateDriverData = &e26r;
    open.PrivateDriverDataSize = sizeof(e26r);
    recordReset();
    device.pfnOpenResource(hDevice, &open, client, rt);
    CHECK(strstr(record(), "open-resource") != nullptr && strstr(record(), "private=64") != nullptr,
          "OpenResource did not reach the hosted driver: %s", record());

    const std::wstring logPath = Layout + L"\\routelogs\\front-" + ExeBase() + L"-" +
                                 std::to_wstring(GetCurrentProcessId()) + L".log";
    auto ask = [&](D3D10DDI_HRESOURCE a, D3D10DDI_HRESOURCE b, UINT flags) {
        BOOL supported = 2;
        device.pfnCheckDirectFlipSupport(hDevice, a, b, flags, &supported);
        CHECK(supported == TRUE || supported == FALSE, "CheckDirectFlipSupport wrote %d", supported);
        return supported;
    };
    auto lastLine = [&]() {
        const std::string log = ReadAll(logPath);
        const size_t end = log.rfind("check_direct_flip");
        return end == std::string::npos ? std::string() : log.substr(end, log.find('\n', end) - end);
    };

    // Negative control 1: no trailer at all (an older kernel driver, or EnableDirectFlipHandshake 0).
    ScanoutTrailer = false;
    CHECK(ask(client, compositor, 0) == FALSE, "TRUE with no scan-out caps trailer");
    CHECK(Has(lastLine(), "answer=0 rule=gated"), "no trailer: %s", lastLine().c_str());
    // Negative control 2: a trailer whose flag is clear. The kernel driver never writes this shape (it
    // writes nothing while the switch is off), so a reader that keys on the magic alone would be wrong.
    ScanoutTrailer = true;
    ScanoutFlags = 0;
    ScanoutWidth = width;
    ScanoutHeight = height;
    CHECK(ask(client, compositor, 0) == FALSE, "TRUE with the DirectFlip flag clear");
    CHECK(Has(lastLine(), "answer=0 rule=gated"), "flag clear: %s", lastLine().c_str());

    // The positive case: the trailer of a start that admits the flip, at this pair's geometry.
    ScanoutFlags = BC250_SCANOUT_CAPS_DIRECT_FLIP;
    CHECK(ask(client, compositor, 0) == TRUE, "the admissible pair was refused: %s", lastLine().c_str());
    const std::string yes = lastLine();
    CHECK(Has(yes, "answer=1 rule=supported caps_query=00000000 caps_flags=00000001 source=1920x1200"),
          "the TRUE line does not carry the answer, the rule and the source mode: %s", yes.c_str());
    const std::string clientPart = "client=" + Ptr(0x41000100) +
                                   " recorded=1 opened=1 primary=1 shared=1 1920x1200 pitch=7680 fmt=87 "
                                   "record_v=3 record_access=5";
    const std::string compositorPart = "compositor=" + Ptr(0x41000000) +
                                       " recorded=1 opened=0 primary=1 shared=0 1920x1200 pitch=7680 fmt=87 "
                                       "record_v=3 record_access=1";
    CHECK(Has(yes, clientPart.c_str()), "the client's half of the line: %s", yes.c_str());
    CHECK(Has(yes, compositorPart.c_str()), "the compositor's half of the line: %s", yes.c_str());
    // IMMEDIATE changes nothing in the rule.
    CHECK(ask(client, compositor, D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE) == TRUE, "TRUE refused for IMMEDIATE");
    // The pair the other way round is never a flip of the wrong buffer.
    CHECK(ask(compositor, client, 0) == FALSE, "TRUE for the pair given the other way round");
    CHECK(Has(lastLine(), "answer=0 rule=sides"), "reversed pair: %s", lastLine().c_str());

    // Negative control 3: the source mode moved (a mode change the trailer reports at the next question).
    // The answer follows the trailer read at this call, not the one the adapter open saw.
    ScanoutWidth = 1920;
    ScanoutHeight = 1080;
    CHECK(ask(client, compositor, 0) == FALSE, "TRUE after the source mode moved to 1920x1080");
    CHECK(Has(lastLine(), "answer=0 rule=source-geometry") && Has(lastLine(), "source=1920x1080"),
          "source mode moved: %s", lastLine().c_str());
    ScanoutWidth = width;
    ScanoutHeight = height;
    CHECK(ask(client, compositor, 0) == TRUE, "TRUE did not come back with the source mode");

    // Negative control 4: the application's record never asked for scan-out (a composed client, which is
    // what the D3D12 shell writes without its scan-out mode, and what the D3D11 shell writes today).
    e26r.Access = BC250_SURFACE_RESOURCE_PRIMARY;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == FALSE, "TRUE for a client record with no SCANOUT bit");
    CHECK(Has(lastLine(), "answer=0 rule=client-scannable"), "no SCANOUT: %s", lastLine().c_str());
    // Negative control 5: the 16-byte v2 record the D3D12 shell wrote before increment 2. The kernel driver
    // admits it, the user-mode rule does not: WddmGdiRecordScannable takes exactly 64 bytes of version 3.
    unsigned long v2[4] = {BC250_SURFACE_RESOURCE_MAGIC, 2ul, 1ul,
                           BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT};
    open.pPrivateDriverData = v2;
    open.PrivateDriverDataSize = sizeof(v2);
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == FALSE, "TRUE for a v2 client record");
    CHECK(Has(lastLine(), "answer=0 rule=client-scannable") && Has(lastLine(), "record_v=2"),
          "v2 record: %s", lastLine().c_str());
    // Negative control 6: a client pitch the hosted driver would not give the compositor's buffer.
    e26r.Access = BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT;
    open.pPrivateDriverData = &e26r;
    open.PrivateDriverDataSize = sizeof(e26r);
    lb7a.Pitch = width * 4 + 256;
    lb7a.Size = (unsigned long long)lb7a.Pitch * height;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == FALSE, "TRUE for two different pitches");
    CHECK(Has(lastLine(), "answer=0 rule=pitch"), "pitch: %s", lastLine().c_str());
    // M15.14 (0.7.216.20): an RGBA8 client, the W3 shape of lab session 458. The blob and the v3 record
    // both say RGBA8. Without PLANE_FORMATS the rule says format; with it the pair flips.
    lb7a.Pitch = width * 4;
    lb7a.Size = (unsigned long long)lb7a.Pitch * height;
    lb7a.Format = D3DDDIFMT_A8B8G8R8;
    e26r.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == FALSE, "TRUE for RGBA8 without PLANE_FORMATS");
    CHECK(Has(lastLine(), "answer=0 rule=format") && Has(lastLine(), "fmt=28"), "RGBA8 no flag: %s",
          lastLine().c_str());
    ScanoutFlags = BC250_SCANOUT_CAPS_DIRECT_FLIP | BC250_SCANOUT_CAPS_PLANE_FORMATS;
    CHECK(ask(other, compositor, 0) == TRUE, "RGBA8 with PLANE_FORMATS refused: %s", lastLine().c_str());
    CHECK(Has(lastLine(), "answer=1 rule=supported") && Has(lastLine(), "caps_flags=00000003") &&
          Has(lastLine(), "pitch=7680 fmt=28"), "RGBA8 with flag: %s", lastLine().c_str());
    CHECK(ask(other, compositor, D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE) == TRUE, "RGBA8 IMMEDIATE refused");
    // The sRGB view of the same storage in the record is the same row.
    e26r.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == TRUE, "an RGBA8 sRGB record refused: %s", lastLine().c_str());
    // A record that names another storage row than the kernel driver's blob is refused, not renamed.
    e26r.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == FALSE, "TRUE for a BGRA8 record over an RGBA8 blob");
    CHECK(Has(lastLine(), "answer=0 rule=format") && Has(lastLine(), "fmt=0 "), "mismatch: %s",
          lastLine().c_str());
    // RGB10A2, the 10-bit swap chain.
    lb7a.Format = D3DDDIFMT_A2B10G10R10;
    e26r.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == TRUE, "RGB10A2 with PLANE_FORMATS refused: %s", lastLine().c_str());
    CHECK(Has(lastLine(), "fmt=24"), "RGB10A2: %s", lastLine().c_str());
    // FP16 at its own 8-byte pitch: not a scan-out row, with or without the flag.
    lb7a.Format = D3DDDIFMT_A16B16G16R16F;
    lb7a.Pitch = width * 8;
    lb7a.Size = (unsigned long long)lb7a.Pitch * height;
    e26r.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device.pfnOpenResource(hDevice, &open, other, rt);
    CHECK(ask(other, compositor, 0) == FALSE, "TRUE for an FP16 client");
    CHECK(Has(lastLine(), "answer=0 rule=format") && Has(lastLine(), "fmt=10"), "FP16: %s", lastLine().c_str());
    ScanoutFlags = BC250_SCANOUT_CAPS_DIRECT_FLIP;
    lb7a.Format = D3DDDIFMT_A8R8G8B8;
    lb7a.Pitch = width * 4;
    lb7a.Size = (unsigned long long)lb7a.Pitch * height;
    e26r.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // Negative control 7: a destroyed client is forgotten, so a reused handle is never answered from the
    // record of a buffer that is gone.
    device.pfnDestroyResource(hDevice, client);
    CHECK(ask(client, compositor, 0) == FALSE, "TRUE for a destroyed client");
    CHECK(Has(lastLine(), "answer=0 rule=record"), "destroyed client: %s", lastLine().c_str());
    ScanoutTrailer = false;
}

static void Child(const std::string &s)
{
    if (s == "policy") { PolicyTests(); return; }
    if (s == "identity") { IdentityTests(); return; }
    if (s == "front-tables") { FrontTableTests(); return; }
    if (s == "front-rule") { FrontRuleTests(); return; }
    if (s == "front-record") { FrontRecordTests(); return; }
    OpenHive(s);

    // Router with doubles. The router lives in <layout>\router next to a double named bc250d3d_zink.dll.
    if (s == "route-not-client") {
        RouterBase(false); Switches(true, true); OverrideHklm();
        HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
        Opened o = Open(r, "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "hosted UMD loaded for a non-client");
        o = Open(r, "OpenAdapter10");
        CHECK(o.hr == S_OK && o.tag == CpuTag10, "OpenAdapter10 hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-client-hosted") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        OverrideHklm();
        HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
        Opened o = Open(r, "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == HostedTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        o = Open(r, "OpenAdapter10");
        CHECK(o.hr == S_OK && o.tag == HostedTag10, "OpenAdapter10 hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"cpu\\bc250d3d.dll"), "CPU UMD loaded on the hosted route");
        std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        CHECK(log.find("entry=OpenAdapter10_2 route=hosted reason=hosted fallback=0 hosted_hr=00000000 hr=00000000") != std::string::npos &&
              log.find("hosted_source=router-directory") != std::string::npos &&
              log.find("entry=OpenAdapter10 route=hosted") != std::string::npos, "route log: %s", log.c_str());
    } else if (s == "route-kill-switch") {
        RouterBase(true); Switches(true, true); SetDw(RouterKey, L"DwmForceCpu", 1); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "hosted UMD loaded despite the kill switch");
    } else if (s == "route-kill-switch-wrong-type") {
        RouterBase(true); Switches(true, true); SetSz(RouterKey, L"DwmForceCpu", L"0"); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-kill-switch-toggle") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        OverrideHklm();
        HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
        Opened a = Open(r, "OpenAdapter10_2");
        SetDw(RouterKey, L"DwmForceCpu", 1);
        Opened b = Open(r, "OpenAdapter10_2");
        SetDw(RouterKey, L"DwmForceCpu", 0);
        Opened c = Open(r, "OpenAdapter10_2");
        CHECK(a.tag == HostedTag102 && b.tag == CpuTag102 && c.tag == HostedTag102, "tags %llu %llu %llu",
              (unsigned long long)a.tag, (unsigned long long)b.tag, (unsigned long long)c.tag);
        std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        CHECK(log.find("route=cpu reason=kill-switch fallback=0 hosted_hr=00000001") != std::string::npos, "route log: %s", log.c_str());
    } else if (s == "route-switches-off") {
        RouterBase(true); Switches(true, false); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "hosted UMD loaded with a switch off");
    } else if (s == "route-switches-absent") {
        RouterBase(true); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-latched-on") {
        // The KMD latched both on at its start; the settings were changed to 0 since (a restart is pending).
        RouterBase(true); Switches(false, false); Latched(0x303);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == HostedTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        CHECK(log.find("switch_source=latched blit=1 interop=1") != std::string::npos, "route log: %s", log.c_str());
    } else if (s == "route-latched-closed") {
        // Requested 1/1 but the KMD closed both at its start (unclean boot, invalid setting): CPU.
        RouterBase(true); Switches(true, true); Latched(0x300); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "hosted UMD loaded with the switches latched closed");
    } else if (s == "route-latched-partial") {
        RouterBase(true); Switches(true, true); Latched(0x301); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-latched-wrong-type") {
        RouterBase(true); Switches(true, true); SetSz(KmdKey, L"InteropLastState", L"771"); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-switches-not-required") {
        RouterBase(true); SetDw(RouterKey, L"RequireKmdSwitches", 0); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == HostedTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-require-wrong-type") {
        RouterBase(true); SetSz(RouterKey, L"RequireKmdSwitches", L"0"); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-hosted-fails") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, HostedUmdPathName, Layout + L"\\fail\\fake-fail.dll");
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        OverrideHklm();
        HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
        Opened o = Open(r, "OpenAdapter10_2");
        // E_UNEXPECTED here would mean the CPU double saw the failed hosted call's scribbles.
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        o = Open(r, "OpenAdapter10");
        CHECK(o.hr == S_OK && o.tag == CpuTag10, "OpenAdapter10 hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        CHECK(log.find("route=cpu reason=hosted fallback=1 hosted_hr=80004005 hr=00000000") != std::string::npos, "route log: %s", log.c_str());
    } else if (s == "route-hosted-missing") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, HostedUmdPathName, Layout + L"\\absent\\bc250d3d_zink.dll");
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-hosted-relative") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, HostedUmdPathName, L"bc250d3d_zink.dll");
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "relative HostedUmdPath fell back to the default");
    } else if (s == "route-other-bitness-paths") {
        // The other bitness's UMD paths name the failing double: the router must not read them (BD-064).
        RouterBase(false); Switches(true, true);
        SetSz(RouterKey, OtherCpuUmdPathName, Layout + L"\\fail\\fake-fail.dll");
        AppBase(L"gpu-default"); SetSz(AppKey, OtherGpuUmdPathName, Layout + L"\\fail\\fake-fail.dll");
        OverrideHklm();
        HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
        Opened o = Open(r, "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == AppTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        o = Open(r, "OpenAdapter10");
        CHECK(o.hr == S_OK && o.tag == CpuTag10, "OpenAdapter10 hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"fail\\fake-fail.dll"), "the other bitness's UMD path was loaded");
        std::string log = AppLog();
        CHECK(Has(log, "route=gpu reason=app-default fallback=0 gpu_hr=00000000") && Has(log, "cpu_source=registry"), "route log: %s", log.c_str());
#ifndef _WIN64
    } else if (s == "route-wow-default-cpu-beside-router") {
        // 32-bit router without CpuUmdPathWow: bc250d3d.dll next to the router (router-wow holds the CPU double).
        Switches(false, false);
        SetSz(RouterKey, L"CpuUmdPath", Layout + L"\\fail\\fake-fail.dll");
        SetSz(AppKey, L"RouteLogDirectory", Layout + L"\\applogs");
        OverrideHklm();
        HMODULE r = LoadAt(L"router-wow\\bc250d3d_router.dll");
        Opened o = Open(r, "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        std::string log = AppLog();
        CHECK(Has(log, "route=cpu reason=app-mode-cpu fallback=0") && Has(log, "cpu_source=router-directory"), "route log: %s", log.c_str());
#endif
    } else if (s == "route-default-cpu-path") {
        // No router key at all: CPU UMD from the compiled default, which does not exist on the host (32-bit router:
        // bc250d3d.dll next to the router, absent from layout\router).
        Switches(false, false); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) || o.hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND),
              "hr=%08lx (expected module or path not found for C:\\BC250\\m15\\desktop-umd173-007\\bc250d3d.dll)", o.hr);
    } else if (s == "route-dwm-name") {
        // Run from <layout>\dwm\dwm.exe: the real process-name rule, no HostedClients.
        RouterBase(false); Switches(true, true); OverrideHklm();
        CHECK(!_wcsicmp(ExeBase().c_str(), L"dwm.exe"), "not running as dwm.exe");
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == HostedTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-dwm-name-kill") {
        RouterBase(false); Switches(true, true); SetDw(RouterKey, L"DwmForceCpu", 1); OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    }

    // The real hosted UMD in <layout>\umd with a placeholder amdgpu_wddm_radv.dll next to it (OpenAdapter only
    // checks that the ICD file exists; it is loaded at CreateDevice, which needs a device).
    else if (s.rfind("umd-", 0) == 0) {
        std::wstring dir = L"umd";
        if (s == "umd-no-icd") dir = L"umd-noicd";
        if (s == "umd-diag-default-dir") dir = L"umd-diag";
        if (s == "umd-registry-icd") SetSz(HostedKey, L"IcdPath", Layout + L"\\icd-alt\\alt-icd.dll");
        if (s == "umd-registry-icd-missing") SetSz(HostedKey, L"IcdPath", Layout + L"\\icd-alt\\missing.dll");
        if (s == "umd-diag-registry") {
            SetDw(HostedKey, L"Diagnostics", 1);
            SetSz(HostedKey, L"DiagnosticsDirectory", Layout + L"\\diaglogs");
        }
        if (s == "umd-diag-default-dir") SetDw(HostedKey, L"Diagnostics", 1);
        if (s == "umd-no-trailer" || s == "umd-env-luid-fallback") QueryMode = Query::NoTrailer;
        if (s == "umd-callback-fails") QueryMode = Query::Fail;
        if (s == "umd-bad-version") QueryMode = Query::BadVersion;
        if (s == "umd-zero-luid") QueryMode = Query::ZeroLuid;
        if (s == "umd-bad-reserved") QueryMode = Query::BadReserved;
        OverrideHklm();
        HMODULE m = LoadAt(dir + L"\\bc250d3d_zink.dll");
        Opened o = Open(m, s == "umd-openadapter10" ? "OpenAdapter10" : "OpenAdapter10_2");
        const bool expectOk = s == "umd-identity" || s == "umd-env-luid-fallback" || s == "umd-env-luid-match" ||
                              s == "umd-registry-icd" || s == "umd-diag-registry" || s == "umd-diag-env" ||
                              s == "umd-diag-default-dir" || s == "umd-openadapter10";
        if (expectOk) {
            CHECK(o.hr == S_OK, "hr=%08lx", o.hr);
            CHECK(o.funcs.pfnCreateDevice && o.funcs.pfnCloseAdapter && o.funcs.pfnCalcPrivateDeviceSize, "adapter table incomplete");
            if (s != "umd-openadapter10")
                CHECK(o.funcs.pfnGetSupportedVersions && o.funcs.pfnGetCaps, "10_2 table incomplete");
            if (o.funcs.pfnCloseAdapter) CHECK(o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
        } else {
            CHECK(FAILED(o.hr), "hr=%08lx, expected a failure", o.hr);
        }
        if (s == "umd-no-trailer") CHECK(o.hr == E_NOINTERFACE, "hr=%08lx", o.hr);
        if (s == "umd-env-luid-mismatch" || s == "umd-env-luid-invalid") CHECK(o.hr == E_INVALIDARG, "hr=%08lx", o.hr);
        if (s == "umd-no-icd" || s == "umd-registry-icd-missing") CHECK(o.hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "hr=%08lx", o.hr);
        if (s == "umd-callback-fails") CHECK(o.hr == E_FAIL, "hr=%08lx", o.hr);
        if (s == "umd-zero-luid") CHECK(o.hr == E_UNEXPECTED, "hr=%08lx", o.hr);
        if (s != "umd-no-icd" && s != "umd-registry-icd-missing") {
            CHECK(QueryCalls == 1 && CapsQuerySize(QuerySize) && QueryHandle == (HANDLE)(UINT_PTR)0x5A5A,
                  "adapter query calls=%u size=%u handle=%p", QueryCalls, QuerySize, QueryHandle);
        } else {
            CHECK(QueryCalls == 0, "an unusable ICD must refuse the adapter before querying it");
        }
        // Diagnostics off (every scenario but the diag ones): no log directory or file may appear.
        const bool diag = s.rfind("umd-diag-", 0) == 0;
        if (!diag) CHECK(!Exists(Layout + L"\\umd\\logs"), "diagnostics off created umd\\logs");
        std::wstring logName = L"\\bc250d3d_zink-" + ExeBase() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log";
        std::wstring logDir = s == "umd-diag-registry" ? Layout + L"\\diaglogs" : s == "umd-diag-env" ? Layout + L"\\diaglogs-env" :
                              Layout + L"\\umd-diag\\logs";
        if (diag) {
            // No fflush here: every completed line must already be in the file for an outside reader.
            std::string log = ReadAll(logDir + logName);
            CHECK(log.find("BC250 hosted UMD config pid=") != std::string::npos && log.find("icd_status=00000000") != std::string::npos &&
                  log.find("BC250 hosted UMD adapter luid=0000000100002a5f source=identity") != std::string::npos,
                  "diagnostics log %ls: %s", (logDir + logName).c_str(), log.c_str());
        }
    }

    // Router in front of the real hosted UMD (<layout>\umd-router: router, real bc250d3d_zink.dll, placeholder ICD).
    else if (s == "stack-hosted" || s == "stack-hosted-no-identity") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        if (s == "stack-hosted-no-identity") QueryMode = Query::NoTrailer;
        OverrideHklm();
        HMODULE r = LoadAt(L"umd-router\\bc250d3d_router.dll");
        Opened o = Open(r, "OpenAdapter10_2");
        std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        if (s == "stack-hosted") {
            CHECK(o.hr == S_OK && Loaded(L"umd-router\\bc250d3d_zink.dll") && !Loaded(L"cpu\\bc250d3d.dll"), "hr=%08lx", o.hr);
            CHECK(o.funcs.pfnCloseAdapter && o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
            CHECK(log.find("route=hosted reason=hosted fallback=0") != std::string::npos, "route log: %s", log.c_str());
        } else {
            // A KMD without the identity trailer: the hosted UMD refuses the adapter and the desktop stays on the CPU UMD.
            CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
            CHECK(log.find("route=cpu reason=hosted fallback=1 hosted_hr=80004002") != std::string::npos, "route log: %s", log.c_str());
        }
    }

    // M15.14: the D3D11_1 front, installed by the router over the hosted UMD's adapter table. The doubles
    // (fake-umd.cpp) are a full D3D10.0 driver here: five adapter entries, 101 device entries and 7 DXGI
    // entries, and they record what the front forwarded.
    else if (s.rfind("front-", 0) == 0) {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        if (s == "front-wrong-type") SetSz(RouterKey, L"DirectFlipFront", L"1");
        else if (s == "front-zero") SetDw(RouterKey, L"DirectFlipFront", 0);
        else if (s != "front-absent") SetDw(RouterKey, L"DirectFlipFront", 1);
        if (s == "front-cpu-route") SetDw(RouterKey, L"DwmForceCpu", 1);
        // front-answer is front-on with the scan-out caps trailer of a start that admits a client flip; the
        // adapter open sees it too, and its install line says so.
        if (s == "front-answer") {
            ScanoutTrailer = true;
            ScanoutWidth = 1920;
            ScanoutHeight = 1200;
        }
        OverrideHklm();
        const bool real = s == "front-stack";
        const std::wstring routerDll = real ? L"umd-router\\bc250d3d_router.dll" : L"router\\bc250d3d_router.dll";
        const std::wstring hostedDll = real ? L"umd-router\\bc250d3d_zink.dll" : L"router\\bc250d3d_zink.dll";
        const bool entry10 = s == "front-d3d10-entry";
        Opened o = Open(LoadAt(routerDll), entry10 ? "OpenAdapter10" : "OpenAdapter10_2");
        const std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        // What the one appended column must say for this scenario.
        // front-absent is "on" from 0.7.213.100-tester.15: the front rides the release, and only the value 0
        // (front-zero) or a value of another kind (front-wrong-type) puts the shipped router back.
        const char *wantColumn = s == "front-wrong-type" ? "front=invalid"
                                 : s == "front-zero" ? "front=off"
                                 : entry10 || s == "front-cpu-route" ? "front=unavailable"
                                                                     : "front=on";
        CHECK(Has(log, wantColumn), "route log has no %s: %s", wantColumn, log.c_str());
        // Every other field of the desktop line is where router 5BBEB783 put it, so the kit's parser still
        // reads it: the front column is appended, never inserted.
        CHECK(Has(log, "switch_source=settings blit=1 interop=1 front="), "the front column is not last: %s",
              log.c_str());
        if (s == "front-cpu-route") {
            CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
            CHECK(!Loaded(hostedDll), "the hosted UMD was loaded although the kill switch is set");
            return;
        }
        CHECK(SUCCEEDED(o.hr), "hr=%08lx", o.hr);
        if (!real) CHECK(o.tag == (entry10 ? HostedTag10 : HostedTag102), "tag=%llu", (unsigned long long)o.tag);
        HMODULE hosted = ModuleAt(hostedDll);
        CHECK(hosted != nullptr, "the hosted UMD is not loaded");
        if (!hosted) return;
        auto record = (const char *(*)(void))GetProcAddress(hosted, "FakeRecord");
        auto recordReset = (void (*)(void))GetProcAddress(hosted, "FakeRecordReset");
        auto fakeSize = (SIZE_T(*)(void))GetProcAddress(hosted, "FakeDeviceSize");
        auto fakeFuncs = (void (*)(D3D10DDI_DEVICEFUNCS *))GetProcAddress(hosted, "FakeDeviceFuncs");
        if (!real) CHECK(record && recordReset && fakeSize && fakeFuncs, "the double has no record exports");

        // The adapter table the runtime now holds. With the front off it is the hosted driver's own, so the
        // version count is the hosted driver's; with the front on it is one longer, and the extra entry is
        // D3D11_1_DDI_SUPPORTED exactly (build version 0).
        const bool on = !strcmp(wantColumn, "front=on");
        if (entry10) {
            // The D3D10.0 entry has no GetSupportedVersions and no GetCaps, and the front needs both: the
            // route is unchanged and CalcPrivateDeviceSize is the hosted driver's own, with no front record
            // behind the device block.
            CHECK(o.funcs.pfnCalcPrivateDeviceSize != nullptr, "no CalcPrivateDeviceSize");
            D3D10DDIARG_CALCPRIVATEDEVICESIZE sizeArgs = {};
            sizeArgs.Interface = D3D10_0_DDI_INTERFACE_VERSION;
            sizeArgs.Version = D3D10_0_DDI_SUPPORTED & 0xFFFFFFFF;
            const SIZE_T size = o.funcs.pfnCalcPrivateDeviceSize(o.adapter, &sizeArgs);
            CHECK(fakeSize && size == fakeSize(), "device size %Iu, expected the hosted driver's own", size);
            return;
        }
        CHECK(o.funcs.pfnGetSupportedVersions && o.funcs.pfnGetCaps, "the 10_2 table is incomplete");
        if (!o.funcs.pfnGetSupportedVersions || !o.funcs.pfnGetCaps) return;
        UINT32 count = 0;
        CHECK(o.funcs.pfnGetSupportedVersions(o.adapter, &count, nullptr) == S_OK, "the count query failed");
        const UINT32 hostedCount = real ? 3 : 2;
        CHECK(count == hostedCount + (on ? 1u : 0u), "%u versions, expected %u", count, hostedCount + (on ? 1u : 0u));
        UINT64 versions[8] = {};
        UINT32 n = 1;
        CHECK(o.funcs.pfnGetSupportedVersions(o.adapter, &n, versions) == E_OUTOFMEMORY,
              "a one-entry array was not refused");
        n = ARRAYSIZE(versions);
        CHECK(o.funcs.pfnGetSupportedVersions(o.adapter, &n, versions) == S_OK, "the fill query failed");
        CHECK(n == count, "the fill query wrote %u entries, the count query said %u", n, count);
        if (on) {
            CHECK(versions[n - 1] == D3D11_1_DDI_SUPPORTED, "the appended version is %016llx, expected %016llx",
                  (unsigned long long)versions[n - 1], (unsigned long long)D3D11_1_DDI_SUPPORTED);
            for (UINT32 i = 0; i + 1 < n; ++i)
                CHECK(versions[i] != D3D11_1_DDI_SUPPORTED, "the hosted list already held D3D11_1");
        } else {
            for (UINT32 i = 0; i < n; ++i)
                CHECK(versions[i] != D3D11_1_DDI_SUPPORTED, "D3D11_1 is offered although the front is off");
        }

        // The caps the front answers itself, and the ones it forwards.
        D3D11DDI_3DPIPELINESUPPORT_CAPS pipeline = {0xFFFFFFFF};
        D3D10_2DDIARG_GETCAPS caps = {};
        caps.Type = D3D11DDICAPS_3DPIPELINESUPPORT;
        caps.pData = &pipeline;
        caps.DataSize = sizeof(pipeline);
        CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == S_OK, "3DPIPELINESUPPORT failed");
        if (on) {
            CHECK(pipeline.Caps == D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0),
                  "3DPIPELINESUPPORT = %08x, expected only level 10_0", pipeline.Caps);
            D3D11DDI_THREADING_CAPS threading = {0xFFFFFFFF};
            caps.Type = D3D11DDICAPS_THREADING;
            caps.pData = &threading;
            caps.DataSize = sizeof(threading);
            CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == S_OK && threading.Caps == 0,
                  "THREADING = %08x, expected 0 (no command lists, no deferred contexts)", threading.Caps);
            D3D11_1DDI_D3D11_OPTIONS_DATA options = {TRUE, TRUE};
            caps.Type = D3D11_1DDICAPS_D3D11_OPTIONS;
            caps.pData = &options;
            caps.DataSize = sizeof(options);
            CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == S_OK && !options.OutputMergerLogicOp &&
                  !options.AssignDebugBinarySupport, "D3D11_OPTIONS is not zeroed");
            // A buffer of another size is a different runtime's view of the structure: refused, never
            // half-written.
            caps.DataSize = sizeof(options) + 4;
            CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == E_INVALIDARG, "a caps buffer of the wrong size");
            // A D3D11-era type the front does not model is answered the way the hosted driver answered every
            // type before the front existed: zeroed, S_OK. An E_INVALIDARG here is a device create that does
            // not happen inside dwm.exe, and every one of these structures means "unsupported" when zeroed.
            UINT32 unknown = 0xFFFFFFFF;
            caps.Type = (D3D10_2DDICAPS_TYPE)136; // D3DWDDM1_3DDICAPS_D3D11_OPTIONS1
            caps.pData = &unknown;
            caps.DataSize = sizeof(unknown);
            CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == S_OK, "an unknown D3D11-era caps type was refused");
            CHECK(unknown == 0, "an unknown caps type did not zero the buffer");
            // And it is answered by the front, not forwarded: the hosted driver's GetCaps sees nothing.
            if (record && recordReset && !real) {
                recordReset();
                unknown = 0xFFFFFFFF;
                caps.Type = (D3D10_2DDICAPS_TYPE)137;
                CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == S_OK, "a second unknown type was refused");
                CHECK(!strstr(record(), "get-caps type=137"),
                      "an unknown D3D11-era type reached the hosted driver: %s", record());
            }
            // A legacy type still goes to the hosted driver, whose zeroing answer is the right one for it.
            if (record && recordReset && !real) {
                recordReset();
                caps.Type = (D3D10_2DDICAPS_TYPE)1;
                CHECK(o.funcs.pfnGetCaps(o.adapter, &caps) == S_OK, "a legacy caps type was refused");
                CHECK(strstr(record(), "get-caps type=1") != nullptr,
                      "a legacy caps type did not reach the hosted driver: %s", record());
            }
        }
        // The blocker of the DDI review, as a scenario: the runtime creates a D3D10.0 device from the list the
        // front appended to. The front must forward that create untouched. If it fills its 11.1 shapes into the
        // runtime's 101-entry table and 7-entry DXGI block instead, it writes 432 and 64 bytes past them and
        // answers S_OK, inside dwm.exe. The two tables here are oversized with a guard pattern, so an overrun
        // is caught by name instead of by a heap crash somewhere else.
        if (s == "front-d3d10-interface") {
            if (!record || !recordReset || !fakeSize || !fakeFuncs) return;
            void *const guard = (void *)(UINT_PTR)0x6A6A6A6A;
            const unsigned deviceSlots = (unsigned)(sizeof(D3D10DDI_DEVICEFUNCS) / sizeof(void *));
            const unsigned dxgiSlots = (unsigned)(sizeof(DXGI_DDI_BASE_FUNCTIONS) / sizeof(void *));
            std::vector<void *> table(deviceSlots + 64, guard);
            std::vector<void *> dxgiTable(dxgiSlots + 16, guard);
            D3D10DDIARG_CALCPRIVATEDEVICESIZE sizeArgs10 = {};
            sizeArgs10.Interface = D3D10_0_DDI_INTERFACE_VERSION;
            sizeArgs10.Version = D3D10_0_DDI_SUPPORTED & 0xFFFFFFFF;
            const SIZE_T size10 = o.funcs.pfnCalcPrivateDeviceSize(o.adapter, &sizeArgs10);
            CHECK(fakeSize && size10 == fakeSize(),
                  "a D3D10.0 private device size is %Iu, expected the hosted driver's own %Iu", size10,
                  fakeSize ? fakeSize() : 0);
            // The block is bigger than the hosted driver asked for, with a pattern behind it: a front record
            // written for a device the front does not front would show up there.
            std::vector<BYTE> block(size10 + 512, 0x3C);
            D3D10DDI_CORELAYER_DEVICECALLBACKS um10 = {};
            PFND3D10DDI_RETRIEVESUBOBJECT retrieve10 = nullptr;
            D3D10DDIARG_CREATEDEVICE create10 = {};
            create10.hRTDevice.handle = (HANDLE)(UINT_PTR)0x7A7B;
            create10.Interface = D3D10_0_DDI_INTERFACE_VERSION;
            create10.Version = D3D10_0_DDI_SUPPORTED & 0xFFFFFFFF;
            create10.pDeviceFuncs = (D3D10DDI_DEVICEFUNCS *)table.data();
            create10.hDrvDevice.pDrvPrivate = block.data();
            create10.DXGIBaseDDI.pDXGIDDIBaseFunctions = (DXGI_DDI_BASE_FUNCTIONS *)dxgiTable.data();
            create10.pUMCallbacks = &um10;
            create10.ppfnRetrieveSubObject = &retrieve10;
            recordReset();
            const HRESULT hr10 = o.funcs.pfnCreateDevice(o.adapter, &create10);
            CHECK(hr10 == DXGI_STATUS_NO_REDIRECTION, "a D3D10.0 create answered %08lx", hr10);
            CHECK(strstr(record(), "create-device\n") != nullptr,
                  "the hosted CreateDevice was not reached: %s", record());
            CHECK(!strstr(record(), "create-device-wrong-interface"),
                  "the front rewrote Interface on a create it must forward: %s", record());
            unsigned past = 0;
            for (unsigned i = deviceSlots; i < table.size(); ++i) if (table[i] != guard) ++past;
            CHECK(past == 0, "%u slots past D3D10DDI_DEVICEFUNCS were written (%u bytes)", past,
                  (unsigned)(past * sizeof(void *)));
            unsigned pastDxgi = 0;
            for (unsigned i = dxgiSlots; i < dxgiTable.size(); ++i) if (dxgiTable[i] != guard) ++pastDxgi;
            CHECK(pastDxgi == 0, "%u slots past DXGI_DDI_BASE_FUNCTIONS were written (%u bytes)", pastDxgi,
                  (unsigned)(pastDxgi * sizeof(void *)));
            // The hosted driver's own table reached the runtime, entry for entry.
            D3D10DDI_DEVICEFUNCS hosted10;
            fakeFuncs(&hosted10);
            unsigned wrong = 0;
            for (unsigned i = 0; i < deviceSlots; ++i)
                if (table[i] != *((void **)&hosted10 + i)) ++wrong;
            CHECK(wrong == 0, "%u of the %u D3D10.0 device entries are not the hosted driver's own", wrong,
                  deviceSlots);
            // And no front record behind the hosted block: the pattern is untouched from the hosted size on.
            unsigned behind = 0;
            for (SIZE_T i = size10; i < block.size(); ++i) if (block[i] != 0x3C) ++behind;
            CHECK(behind == 0, "%u bytes behind the hosted device block were written", behind);
            const std::string log10 = ReadAll(Layout + L"\\routelogs\\front-" + ExeBase() + L"-" +
                                              std::to_wstring(GetCurrentProcessId()) + L".log");
            CHECK(Has(log10, "create_device forwarded interface="),
                  "the front log does not name the forwarded create: %s", log10.c_str());
            CHECK(o.funcs.pfnCloseAdapter && o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
            return;
        }
        if (real) {
            // The front over the REAL hosted UMD, through its device create. Everything below this line used
            // to be checked against fake-hosted.dll alone, which was written to match one reading of the Mesa
            // frontend's Device.cpp: the hosted private size, the 101 entries it fills, how many DXGI entries
            // it really writes and what it returns. This is a host-side call and needs no lab.
            D3D10DDIARG_CALCPRIVATEDEVICESIZE realSize = {};
            realSize.Interface = D3D11_1_DDI_INTERFACE_VERSION;
            realSize.Version = D3D11_1_DDI_SUPPORTED & 0xFFFFFFFF;
            const SIZE_T frontSize = o.funcs.pfnCalcPrivateDeviceSize(o.adapter, &realSize);
            CHECK(frontSize > sizeof(bc250front::Device),
                  "the front's private device size over the real hosted UMD is %Iu", frontSize);
            printf("  real hosted UMD: front private device size %Iu (the front's record is %Iu)\n", frontSize,
                   sizeof(bc250front::Device));
            fflush(stdout);
            // The device create against the REAL hosted UMD needs the GPU: the Mesa frontend's CreateDevice
            // builds a gallium screen over the Vulkan ICD, and on this development host (placeholder ICD, no
            // BC-250) it faults inside the hosted driver before the front sees anything. Measured, not
            // assumed: with the create wired below the scenario died with 0xC0000005 while the same adapter's
            // CalcPrivateDeviceSize answered 10544. So the create is opt-in and runs where a GPU is, and the
            // host gate checks the arithmetic that needs no device.
            const bool realCreate = GetEnvironmentVariableW(L"BC250_ROUTER_REAL_CREATE_DEVICE", nullptr, 0) != 0;
            if (!realCreate)
                printf("  real hosted UMD: CreateDevice not driven here (set BC250_ROUTER_REAL_CREATE_DEVICE "
                       "on a host with the GPU and our KMD)\n");
            if (frontSize && realCreate) {
                void *const guard = (void *)(UINT_PTR)0x6B6B6B6B;
                const unsigned deviceSlots = (unsigned)(sizeof(D3D11_1DDI_DEVICEFUNCS) / sizeof(void *));
                const unsigned dxgiSlots = (unsigned)(sizeof(DXGI1_2_DDI_BASE_FUNCTIONS) / sizeof(void *));
                std::vector<void *> table(deviceSlots + 32, nullptr);
                std::vector<void *> dxgiTable(dxgiSlots + 16, nullptr);
                for (unsigned i = deviceSlots; i < table.size(); ++i) table[i] = guard;
                for (unsigned i = dxgiSlots; i < dxgiTable.size(); ++i) dxgiTable[i] = guard;
                std::vector<BYTE> block(frontSize + 256, 0x2E);
                D3D10DDI_CORELAYER_DEVICECALLBACKS um = {};
                PFND3D10DDI_RETRIEVESUBOBJECT retrieve = nullptr;
                D3D10DDIARG_CREATEDEVICE create = {};
                create.hRTDevice.handle = (HANDLE)(UINT_PTR)0x7A7C;
                create.Interface = D3D11_1_DDI_INTERFACE_VERSION;
                create.Version = D3D11_1_DDI_SUPPORTED & 0xFFFFFFFF;
                create.p11_1DeviceFuncs = (D3D11_1DDI_DEVICEFUNCS *)table.data();
                create.hDrvDevice.pDrvPrivate = block.data();
                create.DXGIBaseDDI.pDXGIDDIBaseFunctions3 = (DXGI1_2_DDI_BASE_FUNCTIONS *)dxgiTable.data();
                create.pUMCallbacks = &um;
                create.ppfnRetrieveSubObject = &retrieve;
                printf("  real hosted UMD: calling CreateDevice\n");
                fflush(stdout);
                const HRESULT realHr = o.funcs.pfnCreateDevice(o.adapter, &create);
                printf("  real hosted UMD: CreateDevice hr=%08lx\n", (unsigned long)realHr);
                fflush(stdout);
                unsigned past = 0, pastDxgi = 0;
                for (unsigned i = deviceSlots; i < table.size(); ++i) if (table[i] != guard) ++past;
                for (unsigned i = dxgiSlots; i < dxgiTable.size(); ++i) if (dxgiTable[i] != guard) ++pastDxgi;
                CHECK(past == 0 && pastDxgi == 0, "%u device and %u DXGI slots past the runtime's tables were "
                      "written over the real hosted UMD", past, pastDxgi);
                unsigned behind = 0;
                for (SIZE_T i = frontSize; i < block.size(); ++i) if (block[i] != 0x2E) ++behind;
                CHECK(behind == 0, "%u bytes behind the front's device block were written", behind);
                if (SUCCEEDED(realHr)) {
                    // A success the lab will see: every published slot must be filled, and how many DXGI
                    // entries the real frontend wrote is the assumption the front's slot-7 rule rests on.
                    unsigned nulls = 0, dxgiNulls = 0, dxgiHosted = 0;
                    for (unsigned i = 0; i < deviceSlots; ++i) if (!table[i]) ++nulls;
                    for (unsigned i = 0; i < dxgiSlots; ++i) if (!dxgiTable[i]) ++dxgiNulls;
                    for (unsigned i = 0; i < 8; ++i) if (dxgiTable[i]) ++dxgiHosted;
                    CHECK(nulls == 0, "%u of the %u device entries are null over the real hosted UMD", nulls,
                          deviceSlots);
                    CHECK(dxgiNulls == 0, "%u DXGI entries are null over the real hosted UMD", dxgiNulls);
                    printf("  real hosted UMD: %u of the first 8 DXGI slots are filled\n", dxgiHosted);
                    D3D11_1DDI_DEVICEFUNCS *funcs = (D3D11_1DDI_DEVICEFUNCS *)table.data();
                    if (funcs->pfnDestroyDevice) funcs->pfnDestroyDevice(create.hDrvDevice);
                } else {
                    // A failure is a legitimate outcome on a host with no GPU: what the scenario then proves
                    // is that the front hands the hosted code back and writes nothing outside the buffers,
                    // which the three checks above have already done.
                    printf("  real hosted UMD: CreateDevice failed with %08lx, which this host may do\n",
                           (unsigned long)realHr);
                }
            }
            CHECK(o.funcs.pfnCloseAdapter && o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
            return;
        }
        if (!on || !record || !recordReset || !fakeSize || !fakeFuncs) {
            CHECK(o.funcs.pfnCloseAdapter && o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
            return;
        }

        // The device. The front's record goes AFTER the hosted driver's block, so the size grows and the
        // hosted block survives untouched.
        recordReset();
        D3D10DDIARG_CALCPRIVATEDEVICESIZE sizeArgs = {};
        sizeArgs.Interface = D3D11_1_DDI_INTERFACE_VERSION;
        sizeArgs.Version = D3D11_1_DDI_SUPPORTED & 0xFFFFFFFF;
        const SIZE_T size = o.funcs.pfnCalcPrivateDeviceSize(o.adapter, &sizeArgs);
        CHECK(size > fakeSize(), "device size %Iu, the hosted driver's own is %Iu", size, fakeSize());
        CHECK(strstr(record(), "calc-private-device-size") != nullptr,
              "the front did not rewrite Interface to the D3D10.0 pair: %s", record());
        std::vector<BYTE> block(size, 0);
        D3D11_1DDI_DEVICEFUNCS device;
        memset(&device, 0, sizeof(device));
        DXGI1_2_DDI_BASE_FUNCTIONS dxgi;
        memset(&dxgi, 0, sizeof(dxgi));
        D3D10DDI_CORELAYER_DEVICECALLBACKS um = {};
        PFND3D10DDI_RETRIEVESUBOBJECT retrieve = nullptr;
        D3D10DDIARG_CREATEDEVICE create = {};
        create.hRTDevice.handle = (HANDLE)(UINT_PTR)0x7A7A;
        create.Interface = D3D11_1_DDI_INTERFACE_VERSION;
        create.Version = D3D11_1_DDI_SUPPORTED & 0xFFFFFFFF;
        create.p11_1DeviceFuncs = &device;
        create.hDrvDevice.pDrvPrivate = block.data();
        create.DXGIBaseDDI.pDXGIDDIBaseFunctions3 = &dxgi;
        create.pUMCallbacks = &um;
        // The 3D pipeline level the runtime encodes in Flags. Level 10_0 is 0, which is the whole point: the
        // front publishes that level alone and every entry it refuses is one such a device cannot reach.
        create.Flags = 0;
        CHECK(D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(create.Flags) == D3D11DDI_3DPIPELINELEVEL_10_0,
              "Flags 0 is not pipeline level 10_0");
        create.ppfnRetrieveSubObject = &retrieve;
        recordReset();
        const HRESULT createHr = o.funcs.pfnCreateDevice(o.adapter, &create);
        // The hosted success code, not S_OK: DXGI_STATUS_NO_REDIRECTION keeps the compositor off the
        // shared-resource presentation path, which is the path this desktop was measured on.
        CHECK(createHr == DXGI_STATUS_NO_REDIRECTION, "CreateDevice hr=%08lx, expected the hosted code %08lx",
              createHr, (unsigned long)DXGI_STATUS_NO_REDIRECTION);
        CHECK(strstr(record(), "create-device\n") != nullptr, "the hosted CreateDevice was not called: %s", record());
        bool pattern = true;
        for (SIZE_T i = 0; i < fakeSize(); ++i) if (block[i] != 0x5D) pattern = false;
        CHECK(pattern, "the front overwrote the hosted driver's own device block");
        // Both published tables are complete: no slot is null and no call lands in address zero.
        unsigned nulls = 0;
        void **deviceSlots = (void **)&device;
        for (unsigned i = 0; i < sizeof(device) / sizeof(void *); ++i) if (!deviceSlots[i]) ++nulls;
        CHECK(nulls == 0, "%u of the %u device entries are null", nulls,
              (unsigned)(sizeof(device) / sizeof(void *)));
        unsigned dxgiNulls = 0;
        void **dxgiSlots = (void **)&dxgi;
        for (unsigned i = 0; i < sizeof(dxgi) / sizeof(void *); ++i) if (!dxgiSlots[i]) ++dxgiNulls;
        CHECK(dxgiNulls == 0, "%u of the %u DXGI entries are null", dxgiNulls,
              (unsigned)(sizeof(dxgi) / sizeof(void *)));
        // The hosted driver's own 7 DXGI entries came back through the union member unchanged.
        D3D10DDI_DEVICEFUNCS hostedFuncs;
        fakeFuncs(&hostedFuncs);
        CHECK((void *)device.pfnDraw == (void *)hostedFuncs.pfnDraw, "pfnDraw is not the hosted entry itself");
        CHECK((void *)device.pfnCheckDirectFlipSupport != nullptr, "pfnCheckDirectFlipSupport is null");

        // M15.14 increment 2: the TRUE answer and its controls, over surfaces the front recorded itself.
        if (s == "front-answer") {
            FrontAnswerChecks(device, create.hDrvDevice, record, recordReset);
            device.pfnDestroyDevice(create.hDrvDevice);
            CHECK(o.funcs.pfnCloseAdapter && o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
            return;
        }
        // The entry this increment exists for: counted, logged, answered. With no scan-out caps trailer in
        // the adapter query (this scenario's kernel driver writes the identity alone, as a start with
        // EnableDirectFlipHandshake 0 does), every answer is FALSE and the first clause says why.
        BOOL supported = TRUE;
        D3D10DDI_HRESOURCE r1, r2;
        r1.pDrvPrivate = (void *)(UINT_PTR)0x40000000;
        r2.pDrvPrivate = (void *)(UINT_PTR)0x40000100;
        device.pfnCheckDirectFlipSupport(create.hDrvDevice, r1, r2, 0, &supported);
        CHECK(supported == FALSE, "CheckDirectFlipSupport answered TRUE with no scan-out caps trailer");
        supported = TRUE;
        device.pfnCheckDirectFlipSupport(create.hDrvDevice, r1, r2, D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE,
                                        &supported);
        CHECK(supported == FALSE, "CheckDirectFlipSupport answered TRUE for an immediate flip");
        device.pfnCheckDirectFlipSupport(create.hDrvDevice, r1, r2, 0, nullptr); // must not fault
        // A null or duplicated handle is `handle`; a handle the front never recorded reaches the next clause,
        // which here is `gated` (no caps trailer) with `recorded=0` beside it. The two facts print
        // differently, which is what a trial reads.
        supported = TRUE;
        D3D10DDI_HRESOURCE rNull;
        rNull.pDrvPrivate = nullptr;
        device.pfnCheckDirectFlipSupport(create.hDrvDevice, rNull, r2, 0, &supported);
        CHECK(supported == FALSE, "a null handle answered TRUE");
        supported = TRUE;
        device.pfnCheckDirectFlipSupport(create.hDrvDevice, r1, r1, 0, &supported);
        CHECK(supported == FALSE, "the same surface twice answered TRUE");
        const std::string frontLog = ReadAll(Layout + L"\\routelogs\\front-" + ExeBase() + L"-" +
                                             std::to_wstring(GetCurrentProcessId()) + L".log");
        CHECK(Has(frontLog, "check_direct_flip call=1") && Has(frontLog, "answer=0") && Has(frontLog, "rule=gated") &&
                  Has(frontLog, "caps_query=00000000 caps_flags=00000000 source=0x0"),
              "the front log has no CheckDirectFlipSupport line for an unrecorded surface: %s", frontLog.c_str());
        const std::string gatedClient = "rule=gated caps_query=00000000 caps_flags=00000000 source=0x0 client=" +
                                        Ptr(0x40000000) + " recorded=0";
        CHECK(Has(frontLog, gatedClient.c_str()),
              "the line does not say that the front had no record for the surface: %s", frontLog.c_str());
        CHECK(Has(frontLog, "rule=handle"), "no line names a null or duplicated handle: %s", frontLog.c_str());
        // The DXGI side. At D3D11_1 the runtime holds pfnBlt1 and prefers it for a stretch, a convert or a
        // resolve present, so this entry is on the desktop's present path: it forwards to the hosted pfnBlt
        // for every shape, names a source rectangle it could not carry, and never refuses.
        recordReset();
        DXGI_DDI_ARG_BLT1 blt = {};
        blt.DstRight = 1920; blt.DstBottom = 1200;
        blt.SrcRight = 1920; blt.SrcBottom = 1200;
        blt.Flags.Present = 1;
        blt.Rotate = DXGI_DDI_MODE_ROTATION_IDENTITY;
        CHECK(dxgi.pfnBlt1 != nullptr, "pfnBlt1 is null");
        CHECK(dxgi.pfnBlt1(&blt) == S_OK, "a whole-surface pfnBlt1 was refused");
        CHECK(strstr(record(), "dxgi-blt dst=0,0,1920,1200") != nullptr,
              "pfnBlt1 did not reach the hosted pfnBlt: %s", record());
        // A rotated blt: DXGI_DDI_ARG_BLT has its own Rotate field, so there is nothing to refuse.
        recordReset();
        blt.Rotate = DXGI_DDI_MODE_ROTATION_ROTATE90;
        CHECK(dxgi.pfnBlt1(&blt) == S_OK, "a rotated pfnBlt1 was refused");
        CHECK(strstr(record(), "rotate=2") != nullptr, "the rotation was not carried: %s", record());
        // A source sub-rectangle: forwarded all the same, and named once in the log.
        recordReset();
        blt.Rotate = DXGI_DDI_MODE_ROTATION_IDENTITY;
        blt.SrcLeft = 16; blt.SrcTop = 8;
        CHECK(dxgi.pfnBlt1(&blt) == S_OK, "a sub-rectangle pfnBlt1 was refused");
        CHECK(strstr(record(), "dxgi-blt dst=0,0,1920,1200") != nullptr,
              "a sub-rectangle pfnBlt1 did not forward: %s", record());
        const std::string bltLog = ReadAll(Layout + L"\\routelogs\\front-" + ExeBase() + L"-" +
                                           std::to_wstring(GetCurrentProcessId()) + L".log");
        CHECK(Has(bltLog, "pfnBlt1 dropped a source rectangle"),
              "the dropped source rectangle is not in the front log: %s", bltLog.c_str());
        // The four multiplane-overlay entries answer DXGI_ERROR_UNSUPPORTED, not E_NOTIMPL, and their lines
        // reach the file through the process-wide log although they have no device record.
        CHECK(dxgi.pfnPresentMultiplaneOverlay(nullptr) == DXGI_ERROR_UNSUPPORTED,
              "pfnPresentMultiplaneOverlay did not answer DXGI_ERROR_UNSUPPORTED");
        const std::string mpoLog = ReadAll(Layout + L"\\routelogs\\front-" + ExeBase() + L"-" +
                                           std::to_wstring(GetCurrentProcessId()) + L".log");
        CHECK(Has(mpoLog, "refused dxgi pfnPresentMultiplaneOverlay"),
              "a DXGI refusal did not reach the front log: %s", mpoLog.c_str());

        // What the front forwards, and what it does not. ClearView with no rectangle list is the whole-view
        // clear the D3D10.0 DDI has; with a rectangle list it cannot be expressed and is counted, not faked.
        recordReset();
        FLOAT color[4] = {0.5f, 0.25f, 0.125f, 1.0f};
        D3D10_DDI_RECT rect = {0, 0, 8, 8};
        device.pfnClearView(create.hDrvDevice, D3D10DDI_HT_RENDERTARGETVIEW, (void *)(UINT_PTR)0x50000000, color,
                            nullptr, 0);
        device.pfnClearView(create.hDrvDevice, D3D10DDI_HT_DEPTHSTENCILVIEW, (void *)(UINT_PTR)0x50000100, color,
                            nullptr, 0);
        device.pfnClearView(create.hDrvDevice, D3D10DDI_HT_RENDERTARGETVIEW, (void *)(UINT_PTR)0x50000000, color,
                            &rect, 1);
        const std::string cleared = record();
        const std::string clearRtv = "clear-rtv view=" + Ptr(0x50000000);
        const std::string clearDsv = "clear-dsv view=" + Ptr(0x50000100) + " flags=1";
        CHECK(strstr(cleared.c_str(), clearRtv.c_str()) != nullptr, "ClearView did not forward: %s",
              cleared.c_str());
        CHECK(strstr(cleared.c_str(), clearDsv.c_str()) != nullptr,
              "ClearView on a depth-stencil view did not forward: %s", cleared.c_str());
        size_t rtvs = 0;
        for (size_t at = cleared.find("clear-rtv"); at != std::string::npos; at = cleared.find("clear-rtv", at + 1))
            ++rtvs;
        CHECK(rtvs == 1, "%Iu whole-view clears forwarded, expected 1: a rectangle list must not be forwarded",
              rtvs);
        // Flush answers TRUE and reaches the hosted driver; RelocateDeviceFuncs re-fills the front's table and
        // must NOT reach it, because the hosted table never moved.
        recordReset();
        CHECK(device.pfnFlush(create.hDrvDevice, 0) == TRUE, "Flush did not answer TRUE");
        CHECK(strstr(record(), "flush") != nullptr, "Flush did not forward: %s", record());
        recordReset();
        D3D11_1DDI_DEVICEFUNCS moved;
        memset(&moved, 0, sizeof(moved));
        device.pfnRelocateDeviceFuncs(create.hDrvDevice, &moved);
        CHECK(!strstr(record(), "relocate-device-funcs"), "RelocateDeviceFuncs reached the hosted driver: %s",
              record());
        unsigned movedNulls = 0;
        void **movedSlots = (void **)&moved;
        for (unsigned i = 0; i < sizeof(moved) / sizeof(void *); ++i) if (!movedSlots[i]) ++movedNulls;
        CHECK(movedNulls == 0, "%u entries are null after RelocateDeviceFuncs", movedNulls);
        // THREADING 0 means no deferred contexts, so there are no handle sizes to report.
        UINT sizes = 0xFFFFFFFF;
        device.pfnCheckDeferredContextHandleSizes(create.hDrvDevice, &sizes, nullptr);
        CHECK(sizes == 0, "CheckDeferredContextHandleSizes reported %u handle sizes", sizes);

        recordReset();
        device.pfnDestroyDevice(create.hDrvDevice);
        CHECK(strstr(record(), "destroy-device") != nullptr, "DestroyDevice did not forward: %s", record());
        // After the device is retired the front still answers, and still answers FALSE.
        supported = TRUE;
        device.pfnCheckDirectFlipSupport(create.hDrvDevice, r1, r2, 0, &supported);
        CHECK(supported == FALSE, "a retired device answered TRUE");
        recordReset();
        CHECK(o.funcs.pfnCloseAdapter && o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
        CHECK(strstr(record(), "close-adapter") != nullptr, "CloseAdapter did not forward: %s", record());
    }

    else if (s == "stack-cpu-real") {
        // Router in front of the deployed CPU UMD build 4176D1DF (<layout>\cpu-real) for a non-client process.
        RouterBase(false); Switches(true, true);
        SetSz(RouterKey, CpuUmdPathName, Layout + L"\\cpu-real\\bc250d3d.dll");
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && Loaded(L"cpu-real\\bc250d3d.dll") && !Loaded(L"router\\bc250d3d_zink.dll"), "hr=%08lx", o.hr);
        CHECK(o.funcs.pfnCreateDevice && o.funcs.pfnCloseAdapter && o.funcs.pfnGetCaps, "CPU UMD adapter table incomplete");
        if (o.funcs.pfnCloseAdapter) CHECK(o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
    }

    // Application policy (AppRouter) with doubles: <layout>\app holds the application double (tag 0xA0) under the
    // shell's file name. The harness itself is the application (test-router.exe), never dwm.exe or a HostedClient.
    else if (s.rfind("app-", 0) == 0) {
        RouterBase(false); Switches(true, true);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        const std::wstring self = ExeBase();
        const char *entry = "OpenAdapter10_2";
        UINT_PTR expect = CpuTag102;
        std::vector<const char *> lines; // fragments the application route line must hold
        if (s == "app-absent-key") {
            // No AppRouter key: every application on the CPU UMD, the line in DesktopRouter's log directory.
            lines = {"route=cpu reason=app-mode-cpu fallback=0 gpu_hr=00000001 hr=00000000", "app_mode=cpu mode_source=key-absent gpu_source=absent cpu_source=registry"};
        } else if (s == "app-mode-absent") {
            AppBase(nullptr); SetMulti(AppKey, L"Allow", {self});
            lines = {"route=cpu reason=app-mode-cpu fallback=0", "app_mode=cpu mode_source=absent gpu_source=registry"};
        } else if (s == "app-mode-cpu") {
            AppBase(L"cpu"); SetMulti(AppKey, L"Allow", {self});
            lines = {"route=cpu reason=app-mode-cpu fallback=0", "app_mode=cpu mode_source=registry"};
        } else if (s == "app-mode-invalid") {
            AppBase(L"gpu"); SetMulti(AppKey, L"Allow", {self});
            lines = {"route=cpu reason=app-mode-invalid fallback=0", "app_mode=invalid mode_source=registry-invalid"};
        } else if (s == "app-mode-wrong-type") {
            AppBase(nullptr); SetDw(AppKey, L"Mode", 1); SetMulti(AppKey, L"Allow", {self});
            lines = {"route=cpu reason=app-mode-invalid fallback=0", "app_mode=invalid mode_source=registry-invalid"};
        } else if (s == "app-allowlist-hit") {
            AppBase(L"allowlist"); SetMulti(AppKey, L"Allow", {L"other.exe", Upper(self)});
            expect = AppTag102;
            lines = {"route=gpu reason=app-allowed fallback=0 gpu_hr=00000000 hr=00000000", "app_mode=allowlist mode_source=registry gpu_source=registry"};
        } else if (s == "app-allowlist-miss") {
            AppBase(L"allowlist"); SetMulti(AppKey, L"Allow", {L"other.exe", L"x" + self});
            lines = {"route=cpu reason=app-not-allowed fallback=0"};
        } else if (s == "app-allowlist-empty") {
            AppBase(L"allowlist");
            lines = {"route=cpu reason=app-not-allowed fallback=0"};
        } else if (s == "app-deny-wins") {
            AppBase(L"allowlist"); SetMulti(AppKey, L"Allow", {self}); SetMulti(AppKey, L"Deny", {L"other.exe", self});
            lines = {"route=cpu reason=app-denied fallback=0"};
        } else if (s == "app-gpu-default") {
            AppBase(L"GPU-Default"); SetMulti(AppKey, L"Deny", {L"other.exe"});
            expect = AppTag102;
            lines = {"route=gpu reason=app-default fallback=0 gpu_hr=00000000", "app_mode=gpu-default mode_source=registry"};
        } else if (s == "app-gpu-default-denied") {
            AppBase(L"gpu-default"); SetMulti(AppKey, L"Deny", {L"other.exe", Upper(self)});
            lines = {"route=cpu reason=app-denied fallback=0"};
        } else if (s == "app-deny-wrong-type") {
            // A Deny list that cannot be read must not widen the GPU route.
            AppBase(L"gpu-default"); SetSz(AppKey, L"Deny", L"other.exe");
            lines = {"route=cpu reason=app-mode-invalid fallback=0", "app_mode=invalid mode_source=list-invalid"};
        } else if (s == "app-allow-wrong-type") {
            AppBase(L"allowlist"); SetSz(AppKey, L"Allow", self);
            lines = {"route=cpu reason=app-mode-invalid fallback=0", "mode_source=list-invalid"};
        } else if (s == "app-gpu-umd-unset") {
            AppBase(L"gpu-default", nullptr);
            lines = {"route=cpu reason=app-gpu-umd-unset fallback=0", "gpu_source=absent"};
        } else if (s == "app-gpu-umd-relative") {
            AppBase(L"gpu-default", nullptr); SetSz(AppKey, GpuUmdPathName, L"amdgpu_wddm_d3d11.dll");
            lines = {"route=cpu reason=app-gpu-umd-unset fallback=0", "gpu_source=registry-invalid"};
        } else if (s == "app-gpu-umd-wrong-type") {
            AppBase(L"gpu-default", nullptr); SetDw(AppKey, GpuUmdPathName, 1);
            lines = {"route=cpu reason=app-gpu-umd-unset fallback=0", "gpu_source=registry-invalid"};
        } else if (s == "app-gpu-fails") {
            // E_UNEXPECTED here would mean the CPU double saw the failed GPU call's scribbles.
            AppBase(L"gpu-default", L"fail\\fake-fail.dll");
            lines = {"route=cpu reason=app-default fallback=1 gpu_hr=80004005 hr=00000000"};
        } else if (s == "app-gpu-missing") {
            AppBase(L"gpu-default", L"absent\\amdgpu_wddm_d3d11.dll");
            lines = {"route=cpu reason=app-default fallback=1 gpu_hr=8007", "hr=00000000"};
        } else if (s == "app-gpu-no-export") {
            AppBase(L"allowlist", L"app-noexport\\amdgpu_wddm_d3d11.dll"); SetMulti(AppKey, L"Allow", {self});
            lines = {"route=cpu reason=app-allowed fallback=1 gpu_hr=80004002 hr=00000000"};
        } else if (s == "app-d3d10-entry") {
            AppBase(L"gpu-default");
            entry = "OpenAdapter10";
            expect = CpuTag10;
            lines = {"entry=OpenAdapter10 route=cpu reason=app-d3d10-entry fallback=0"};
        } else if (s == "app-log-dir-fallback") {
            // No AppRouter RouteLogDirectory: the line goes to DesktopRouter's.
            SetSz(AppKey, L"Mode", L"gpu-default"); SetSz(AppKey, GpuUmdPathName, Layout + L"\\app\\amdgpu_wddm_d3d11.dll");
            expect = AppTag102;
            lines = {"route=gpu reason=app-default fallback=0"};
        } else if (s == "app-mode-toggle") {
            AppBase(L"allowlist"); SetMulti(AppKey, L"Allow", {self});
            OverrideHklm();
            HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
            Opened a = Open(r, "OpenAdapter10_2");
            SetSz(AppKey, L"Mode", L"cpu");
            Opened b = Open(r, "OpenAdapter10_2");
            SetSz(AppKey, L"Mode", L"gpu-default");
            Opened c = Open(r, "OpenAdapter10_2");
            DeleteValue(AppKey, L"Mode");
            Opened d = Open(r, "OpenAdapter10_2");
            CHECK(a.tag == AppTag102 && b.tag == CpuTag102 && c.tag == AppTag102 && d.tag == CpuTag102, "tags %llu %llu %llu %llu",
                  (unsigned long long)a.tag, (unsigned long long)b.tag, (unsigned long long)c.tag, (unsigned long long)d.tag);
            std::string log = AppLog();
            CHECK(Has(log, "reason=app-allowed") && Has(log, "reason=app-mode-cpu fallback=0 gpu_hr=00000001") &&
                  Has(log, "reason=app-default") && Has(log, "mode_source=absent"), "route log: %s", log.c_str());
            return;
        } else {
            printf("  unknown scenario\n");
            ++failures;
            return;
        }
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), entry);
        CHECK(o.hr == S_OK && o.tag == expect, "hr=%08lx tag=%llu expected %llu", o.hr, (unsigned long long)o.tag, (unsigned long long)expect);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "hosted UMD loaded for an application");
        if (expect == AppTag102) CHECK(!Loaded(L"cpu\\bc250d3d.dll"), "CPU UMD loaded on the application GPU route");
        if (expect != AppTag102)
            CHECK(!Loaded(L"app\\amdgpu_wddm_d3d11.dll"), "application GPU UMD loaded on a CPU decision");
        const bool desktopDir = s == "app-absent-key" || s == "app-log-dir-fallback";
        std::string log = desktopDir ? ReadAll(RouteLog(Layout + L"\\routelogs")) : AppLog();
        for (const char *l : lines) CHECK(Has(log, l), "route log lacks '%s': %s", l, log.c_str());
        CHECK(Has(log, "bc250d3d_router pid=") && !Has(log, "hosted_hr="), "not an application line: %s", log.c_str());
        if (!desktopDir) CHECK(!Exists(RouteLog(Layout + L"\\routelogs")), "application line in DesktopRouter's log directory");
    }

    // The desktop is not touched by the application policy: dwm.exe (run as <layout>\dwm\dwm.exe) and a HostedClients
    // entry keep the 5BBEB783 routes and line, whatever AppRouter says.
    else if (s == "desktop-dwm-unchanged" || s == "desktop-client-unchanged") {
        const bool dwm = s == "desktop-dwm-unchanged";
        RouterBase(!dwm); Switches(true, true);
        SetSz(RouterKey, L"RouteLogDirectory", Layout + L"\\routelogs");
        AppBase(L"gpu-default"); SetMulti(AppKey, L"Allow", {L"dwm.exe", ExeBase()});
        OverrideHklm();
        if (dwm) CHECK(!_wcsicmp(ExeBase().c_str(), L"dwm.exe"), "not running as dwm.exe");
        HMODULE r = LoadAt(L"router\\bc250d3d_router.dll");
        Opened a = Open(r, "OpenAdapter10_2");
        SetDw(RouterKey, L"DwmForceCpu", 1);
        Opened b = Open(r, "OpenAdapter10_2");
        SetDw(RouterKey, L"DwmForceCpu", 0); Switches(true, false);
        Opened c = Open(r, "OpenAdapter10_2");
        CHECK(a.tag == HostedTag102 && b.tag == CpuTag102 && c.tag == CpuTag102, "tags %llu %llu %llu",
              (unsigned long long)a.tag, (unsigned long long)b.tag, (unsigned long long)c.tag);
        CHECK(!Loaded(L"app\\amdgpu_wddm_d3d11.dll"), "application GPU UMD loaded for the desktop");
        std::string log = ReadAll(RouteLog(Layout + L"\\routelogs"));
        CHECK(Has(log, "route=hosted reason=hosted fallback=0 hosted_hr=00000000") && Has(log, "route=cpu reason=kill-switch fallback=0") &&
              Has(log, "route=cpu reason=kmd-switches-off fallback=0") && !Has(log, "app_mode=") && !Has(log, "gpu_hr="),
              "route log: %s", log.c_str());
        CHECK(!Exists(RouteLog(Layout + L"\\applogs")), "desktop line in AppRouter's log directory");
    } else if (s == "protected-logonui") {
        // Run from <layout>\logonui\logonui.exe: listed in Allow, gpu-default, still the CPU UMD.
        RouterBase(false); Switches(true, true);
        AppBase(L"gpu-default"); SetMulti(AppKey, L"Allow", {L"logonui.exe"});
        OverrideHklm();
        CHECK(!_wcsicmp(ExeBase().c_str(), L"logonui.exe"), "not running as logonui.exe");
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"app\\amdgpu_wddm_d3d11.dll"), "application GPU UMD loaded for logonui.exe");
        std::string log = AppLog();
        CHECK(Has(log, "route=cpu reason=app-protected fallback=0"), "route log: %s", log.c_str());
    }

    // Router in front of the real DXVK shell E748418C (<layout>\app-real: shell and its config A9B498ED; the engine
    // and the ICD are loaded only at CreateDevice). The CPU UMD is the double, so a fallback shows as its tag.
    else if (s.rfind("stack-app-real", 0) == 0) {
        RouterBase(false); Switches(true, true);
        const bool noconfig = s == "stack-app-real-noconfig";
        AppBase(L"allowlist", noconfig ? L"app-real-noconfig\\amdgpu_wddm_d3d11.dll" : L"app-real\\amdgpu_wddm_d3d11.dll");
        SetMulti(AppKey, L"Allow", {ExeBase()});
        if (s == "stack-app-real-no-identity") QueryMode = Query::NoTrailer;
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        std::string log = AppLog();
        if (s == "stack-app-real") {
            CHECK(o.hr == S_OK && Loaded(L"app-real\\amdgpu_wddm_d3d11.dll") && !Loaded(L"cpu\\bc250d3d.dll"), "hr=%08lx", o.hr);
            CHECK(o.funcs.pfnCreateDevice && o.funcs.pfnCloseAdapter && o.funcs.pfnCalcPrivateDeviceSize &&
                  o.funcs.pfnGetSupportedVersions && o.funcs.pfnGetCaps, "shell adapter table incomplete");
            // The DXVK shell sizes its query from the last trailer in driver/contract, so that a kernel
            // driver which writes a trailer only when all of it fits can reach it. CapsQuerySize accepts
            // either length where the caller may be the hosted UMD, which reads the identity alone; here
            // the caller is the shell, so the gate names the one length it must ask for.
            CHECK(QueryCalls == 1 && QuerySize == BC250_SCANOUT_CAPS_TOTAL && QueryHandle == (HANDLE)(UINT_PTR)0x5A5A,
                  "adapter query calls=%u size=%u handle=%p", QueryCalls, QuerySize, QueryHandle);
            if (o.funcs.pfnCloseAdapter) CHECK(o.funcs.pfnCloseAdapter(o.adapter) == S_OK, "CloseAdapter");
            CHECK(Has(log, "route=gpu reason=app-allowed fallback=0 gpu_hr=00000000 hr=00000000"), "route log: %s", log.c_str());
        } else if (noconfig) {
            CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
            CHECK(QueryCalls == 0, "the shell queried the adapter without a config");
            CHECK(Has(log, "route=cpu reason=app-allowed fallback=1 gpu_hr=80070002 hr=00000000"), "route log: %s", log.c_str());
        } else {
            // A KMD without the identity trailer: the shell refuses the adapter, the application stays on the CPU UMD.
            CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
            CHECK(Has(log, "route=cpu reason=app-allowed fallback=1 gpu_hr=887a0004 hr=00000000"), "route log: %s", log.c_str());
        }
    } else {
        printf("  unknown scenario\n");
        ++failures;
    }
}

// ---------------------------------------------------------------- parent

struct Scenario { const char *name; const char *exe; std::vector<std::pair<std::wstring, std::wstring>> env; };

static int RunAll(const std::wstring &out)
{
    const std::wstring hex = L"100002a5f";
    const std::vector<Scenario> scenarios = {
        {"policy", nullptr, {}}, {"identity", nullptr, {}},
        {"route-not-client", nullptr, {}}, {"route-client-hosted", nullptr, {}}, {"route-kill-switch", nullptr, {}},
        {"route-kill-switch-wrong-type", nullptr, {}}, {"route-kill-switch-toggle", nullptr, {}},
        {"route-switches-off", nullptr, {}}, {"route-switches-absent", nullptr, {}},
        {"route-latched-on", nullptr, {}}, {"route-latched-closed", nullptr, {}}, {"route-latched-partial", nullptr, {}},
        {"route-latched-wrong-type", nullptr, {}},
        {"route-switches-not-required", nullptr, {}}, {"route-require-wrong-type", nullptr, {}},
        {"route-hosted-fails", nullptr, {}}, {"route-hosted-missing", nullptr, {}}, {"route-hosted-relative", nullptr, {}},
        {"route-default-cpu-path", nullptr, {}}, {"route-other-bitness-paths", nullptr, {}},
#ifndef _WIN64
        {"route-wow-default-cpu-beside-router", nullptr, {}},
#endif
        {"route-dwm-name", "dwm\\dwm.exe", {}}, {"route-dwm-name-kill", "dwm\\dwm.exe", {}},
        {"umd-identity", nullptr, {}}, {"umd-openadapter10", nullptr, {}},
        {"umd-no-trailer", nullptr, {}}, {"umd-callback-fails", nullptr, {}}, {"umd-bad-version", nullptr, {}},
        {"umd-zero-luid", nullptr, {}}, {"umd-bad-reserved", nullptr, {}},
        {"umd-env-luid-fallback", nullptr, {{L"BC250_D3D_ZINK_LUID", hex}}},
        {"umd-env-luid-match", nullptr, {{L"BC250_D3D_ZINK_LUID", hex}}},
        {"umd-env-luid-mismatch", nullptr, {{L"BC250_D3D_ZINK_LUID", L"100002a60"}}},
        {"umd-env-luid-invalid", nullptr, {{L"BC250_D3D_ZINK_LUID", L"0x100002a5f"}}},
        {"umd-no-icd", nullptr, {}}, {"umd-registry-icd", nullptr, {}}, {"umd-registry-icd-missing", nullptr, {}},
        {"umd-diag-registry", nullptr, {}}, {"umd-diag-default-dir", nullptr, {}},
        {"umd-diag-env", nullptr, {{L"BC250_UMD_DIAG", L"1"}, {L"BC250_UMD_DIAG_DIR", L"<layout>\\diaglogs-env"}}},
        {"stack-hosted", nullptr, {}}, {"stack-hosted-no-identity", nullptr, {}}, {"stack-cpu-real", nullptr, {}},
        // M15.14: the D3D11_1 front. Three pure suites and one scenario per state of DirectFlipFront.
        {"front-tables", nullptr, {}}, {"front-rule", nullptr, {}}, {"front-record", nullptr, {}},
        {"front-absent", nullptr, {}}, {"front-zero", nullptr, {}}, {"front-wrong-type", nullptr, {}},
        {"front-on", nullptr, {}}, {"front-d3d10-entry", nullptr, {}}, {"front-cpu-route", nullptr, {}},
        {"front-d3d10-interface", nullptr, {}}, {"front-stack", nullptr, {}}, {"front-answer", nullptr, {}},
        // Application policy (AppRouter).
        {"app-absent-key", nullptr, {}}, {"app-mode-absent", nullptr, {}}, {"app-mode-cpu", nullptr, {}},
        {"app-mode-invalid", nullptr, {}}, {"app-mode-wrong-type", nullptr, {}},
        {"app-allowlist-hit", nullptr, {}}, {"app-allowlist-miss", nullptr, {}}, {"app-allowlist-empty", nullptr, {}},
        {"app-deny-wins", nullptr, {}}, {"app-gpu-default", nullptr, {}}, {"app-gpu-default-denied", nullptr, {}},
        {"app-deny-wrong-type", nullptr, {}}, {"app-allow-wrong-type", nullptr, {}},
        {"app-gpu-umd-unset", nullptr, {}}, {"app-gpu-umd-relative", nullptr, {}}, {"app-gpu-umd-wrong-type", nullptr, {}},
        {"app-gpu-fails", nullptr, {}}, {"app-gpu-missing", nullptr, {}}, {"app-gpu-no-export", nullptr, {}},
        {"app-d3d10-entry", nullptr, {}}, {"app-log-dir-fallback", nullptr, {}}, {"app-mode-toggle", nullptr, {}},
        {"protected-logonui", "logonui\\logonui.exe", {}},
        {"desktop-dwm-unchanged", "dwm\\dwm.exe", {}}, {"desktop-client-unchanged", nullptr, {}},
        {"stack-app-real", nullptr, {}}, {"stack-app-real-noconfig", nullptr, {}}, {"stack-app-real-no-identity", nullptr, {}},
    };
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    int failed = 0;
    FILE *summary = nullptr;
    _wfopen_s(&summary, (out + L"\\summary.txt").c_str(), L"w");
    for (const Scenario &sc : scenarios) {
        std::wstring name(sc.name, sc.name + strlen(sc.name));
        std::wstring exe = sc.exe ? Layout + L"\\" + std::wstring(sc.exe, sc.exe + strlen(sc.exe)) : self;
        std::wstring cmd = L"\"" + exe + L"\" child " + name + L" \"" + Layout + L"\"";
        // Environment: the parent's without any BC250_ variable, plus the scenario's.
        std::vector<std::wstring> vars;
        for (wchar_t *e = GetEnvironmentStringsW(), *p = e; *p; p += wcslen(p) + 1)
            if (_wcsnicmp(p, L"BC250_", 6)) vars.push_back(p);
        for (auto &kv : sc.env) {
            std::wstring v = kv.second;
            size_t at = v.find(L"<layout>");
            if (at != std::wstring::npos) v.replace(at, 8, Layout);
            vars.push_back(kv.first + L"=" + v);
        }
        std::wstring block;
        for (auto &v : vars) { block += v; block.push_back(0); }
        block.push_back(0);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE o = CreateFileW((out + L"\\" + name + L".out").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        HANDLE e = CreateFileW((out + L"\\" + name + L".err").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = o;
        si.hStdError = e;
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
        cmdline.push_back(0);
        DWORD code = 99;
        if (CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                           block.data(), nullptr, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 98); WaitForSingleObject(pi.hProcess, 5000); }
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        CloseHandle(o);
        CloseHandle(e);
        LARGE_INTEGER errSize = {};
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (GetFileAttributesExW((out + L"\\" + name + L".err").c_str(), GetFileExInfoStandard, &fa)) {
            errSize.LowPart = fa.nFileSizeLow; errSize.HighPart = (LONG)fa.nFileSizeHigh;
        }
        // Nothing may reach stderr: with diagnostics off the UMD writes nowhere, with them on only to its log file.
        const bool pass = code == 0 && errSize.QuadPart == 0;
        if (!pass) ++failed;
        printf("%s %s exit=%lu stderr_bytes=%lld\n", pass ? "PASS" : "FAIL", sc.name, code, errSize.QuadPart);
        if (summary) fprintf(summary, "%s %s exit=%lu stderr_bytes=%lld\n", pass ? "PASS" : "FAIL", sc.name, code, errSize.QuadPart);
    }
    printf("%d scenarios, %d failed\n", (int)scenarios.size(), failed);
    if (summary) { fprintf(summary, "%d scenarios, %d failed\n", (int)scenarios.size(), failed); fclose(summary); }
    return failed ? 1 : 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 4 && !wcscmp(argv[1], L"run")) {
        Layout = argv[2];
        return RunAll(argv[3]);
    }
    if (argc == 4 && !wcscmp(argv[1], L"child")) {
        Layout = argv[3];
        std::string s;
        for (const wchar_t *p = argv[2]; *p; ++p) s.push_back(static_cast<char>(*p)); // ASCII scenario names
        Child(s);
        printf("%s %s\n", failures ? "FAIL" : "PASS", s.c_str());
        return failures ? 1 : 0;
    }
    printf("usage: test-router run <layout> <out> | child <scenario> <layout>\n");
    return 2;
}
