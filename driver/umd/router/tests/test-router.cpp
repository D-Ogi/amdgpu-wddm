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
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <share.h>
#include <string>
#include <vector>
#include "../router-policy.h"
#include "bc250_adapter_identity.h" // driver/contract (build.ps1 /I)

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

    // Windows components (gpu-default only; Allow overrides; Deny and the protected list still win).
    const wchar_t allowDx[] = L"dxdiag.exe\0";
    struct CompCase { const wchar_t *exe; AppMode mode; const wchar_t *allow, *deny; bool component; AppRoute route; AppReason reason; };
    const CompCase comp[] = {
        {L"notepad.exe", Gd, allowDx, nullptr, true, AppRoute::Cpu, AppReason::WindowsComponent},
        {L"dxdiag.exe", Gd, allowDx, nullptr, true, AppRoute::Gpu, AppReason::Default},
        {L"game.exe", Gd, allowDx, nullptr, false, AppRoute::Gpu, AppReason::Default},
        {L"witcher3.exe", Gd, allowDx, deny, false, AppRoute::Cpu, AppReason::Denied},
        {L"dxdiag.exe", Gd, allowDx, L"dxdiag.exe\0", true, AppRoute::Cpu, AppReason::Denied},
        {L"logonui.exe", Gd, protectedAllowed, nullptr, true, AppRoute::Cpu, AppReason::Protected},
        {L"notepad.exe", Al, allowDx, nullptr, true, AppRoute::Cpu, AppReason::NotAllowed},
        {L"dxdiag.exe", Al, allowDx, nullptr, true, AppRoute::Gpu, AppReason::Allowed},
    };
    for (const CompCase &c : comp) {
        AppDecision d = DecideApp({c.exe, c.mode, c.allow, c.deny, true, true, c.component});
        CHECK(d.route == c.route && d.reason == c.reason, "component exe=%ls mode=%d comp=%d got %s",
              c.exe, (int)c.mode, c.component, AppReasonName(d.reason));
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
static UINT64 QueryLuid = 0x0000000100002A5FULL;
static unsigned QueryCalls;
static UINT QuerySize;
static HANDLE QueryHandle;

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
    if (gpu) SetSz(AppKey, L"GpuUmdPath", Layout + L"\\" + gpu);
    SetSz(AppKey, L"RouteLogDirectory", Layout + L"\\applogs");
}
static std::string AppLog() { return ReadAll(RouteLog(Layout + L"\\applogs")); }
static bool Has(const std::string &log, const char *text) { return log.find(text) != std::string::npos; }

// Common router key: fake CPU UMD, hosted UMD from the router's own directory (no HostedUmdPath).
static void RouterBase(bool clientIsSelf)
{
    SetSz(RouterKey, L"CpuUmdPath", Layout + L"\\cpu\\bc250d3d.dll");
    SetDw(RouterKey, L"DwmForceCpu", 0);
    SetDw(RouterKey, L"RequireKmdSwitches", 1);
    if (clientIsSelf) SetMulti(RouterKey, L"HostedClients", {L"unrelated.exe", ExeBase()});
}

// ---------------------------------------------------------------- scenarios

static void Child(const std::string &s)
{
    if (s == "policy") { PolicyTests(); return; }
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
        SetSz(RouterKey, L"HostedUmdPath", Layout + L"\\fail\\fake-fail.dll");
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
        SetSz(RouterKey, L"HostedUmdPath", Layout + L"\\absent\\bc250d3d_zink.dll");
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
    } else if (s == "route-hosted-relative") {
        RouterBase(true); Switches(true, true);
        SetSz(RouterKey, L"HostedUmdPath", L"bc250d3d_zink.dll");
        OverrideHklm();
        Opened o = Open(LoadAt(L"router\\bc250d3d_router.dll"), "OpenAdapter10_2");
        CHECK(o.hr == S_OK && o.tag == CpuTag102, "hr=%08lx tag=%llu", o.hr, (unsigned long long)o.tag);
        CHECK(!Loaded(L"router\\bc250d3d_zink.dll"), "relative HostedUmdPath fell back to the default");
    } else if (s == "route-default-cpu-path") {
        // No router key at all: CPU UMD from the compiled default, which does not exist on the host.
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
            CHECK(QueryCalls == 1 && QuerySize == BC250_ADAPTER_CAPS_BYTES && QueryHandle == (HANDLE)(UINT_PTR)0x5A5A,
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
    } else if (s == "stack-cpu-real") {
        // Router in front of the deployed CPU UMD build 4176D1DF (<layout>\cpu-real) for a non-client process.
        RouterBase(false); Switches(true, true);
        SetSz(RouterKey, L"CpuUmdPath", Layout + L"\\cpu-real\\bc250d3d.dll");
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
            AppBase(L"gpu-default", nullptr); SetSz(AppKey, L"GpuUmdPath", L"amdgpu_wddm_d3d11.dll");
            lines = {"route=cpu reason=app-gpu-umd-unset fallback=0", "gpu_source=registry-invalid"};
        } else if (s == "app-gpu-umd-wrong-type") {
            AppBase(L"gpu-default", nullptr); SetDw(AppKey, L"GpuUmdPath", 1);
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
            SetSz(AppKey, L"Mode", L"gpu-default"); SetSz(AppKey, L"GpuUmdPath", Layout + L"\\app\\amdgpu_wddm_d3d11.dll");
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
            CHECK(QueryCalls == 1 && QuerySize == BC250_ADAPTER_CAPS_BYTES && QueryHandle == (HANDLE)(UINT_PTR)0x5A5A,
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
        {"policy", nullptr, {}},
        {"route-not-client", nullptr, {}}, {"route-client-hosted", nullptr, {}}, {"route-kill-switch", nullptr, {}},
        {"route-kill-switch-wrong-type", nullptr, {}}, {"route-kill-switch-toggle", nullptr, {}},
        {"route-switches-off", nullptr, {}}, {"route-switches-absent", nullptr, {}},
        {"route-latched-on", nullptr, {}}, {"route-latched-closed", nullptr, {}}, {"route-latched-partial", nullptr, {}},
        {"route-latched-wrong-type", nullptr, {}},
        {"route-switches-not-required", nullptr, {}}, {"route-require-wrong-type", nullptr, {}},
        {"route-hosted-fails", nullptr, {}}, {"route-hosted-missing", nullptr, {}}, {"route-hosted-relative", nullptr, {}},
        {"route-default-cpu-path", nullptr, {}},
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
