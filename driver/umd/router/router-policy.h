// SPDX-License-Identifier: MIT
// Route decisions of bc250d3d_router.dll, free of I/O so that the host tests can drive them directly.
//
// Two independent decisions, taken in this order for every OpenAdapter call:
//  1. Desktop (unchanged from router 5BBEB783): dwm.exe and HostedClients go to the hosted GPU UMD or, by the kill
//     switch or the KMD switches, to the CPU UMD. Decide() below.
//  2. Applications (every process the desktop decision calls NotHostedClient): the AppRouter policy chooses between
//     the application GPU UMD (the DXVK-based amdgpu_wddm_d3d11.dll) and the CPU UMD. DecideApp() below.
#pragma once
#include <cwchar>

namespace bc250router {

enum class Route { Cpu, Hosted };

enum class Reason {
    NotHostedClient,  // the process is neither dwm.exe nor a HostedClients entry
    KillSwitch,       // DesktopRouter DwmForceCpu is a DWORD other than 0
    SwitchesOff,      // RequireKmdSwitches is on and the two KMD desktop switches are not both 1
    Hosted,           // all conditions met: the hosted GPU UMD
};

// Everything the decision reads, as the router found it for one OpenAdapter call.
struct Inputs {
    const wchar_t *exe_base;        // base name of the process image, e.g. L"dwm.exe"
    const wchar_t *hosted_clients;  // REG_MULTI_SZ block (NUL-separated, double NUL) or nullptr
    unsigned long force_cpu;        // DwmForceCpu; absent = 0
    unsigned long require_switches; // RequireKmdSwitches; absent = 1
    bool blit_on;                   // EnableGpuPresentBlit is a DWORD equal to 1
    bool interop_on;                // EnableCddDwmInterop is a DWORD equal to 1
};

struct Decision {
    Route route;
    Reason reason;
};

// Case-insensitive exact match of a base name against a REG_MULTI_SZ block.
inline bool InList(const wchar_t *exe_base, const wchar_t *list)
{
    if (!exe_base || !*exe_base) return false;
    for (const wchar_t *c = list; c && *c; c += wcslen(c) + 1)
        if (!_wcsicmp(exe_base, c)) return true;
    return false;
}

inline bool IsHostedClient(const wchar_t *exe_base, const wchar_t *clients)
{
    if (!exe_base || !*exe_base) return false;
    if (!_wcsicmp(exe_base, L"dwm.exe")) return true;
    return InList(exe_base, clients);
}

inline Decision Decide(const Inputs &in)
{
    if (!IsHostedClient(in.exe_base, in.hosted_clients)) return {Route::Cpu, Reason::NotHostedClient};
    if (in.force_cpu) return {Route::Cpu, Reason::KillSwitch};
    if (in.require_switches && !(in.blit_on && in.interop_on)) return {Route::Cpu, Reason::SwitchesOff};
    return {Route::Hosted, Reason::Hosted};
}

inline const char *ReasonName(Reason r)
{
    switch (r) {
    case Reason::NotHostedClient: return "not-hosted-client";
    case Reason::KillSwitch: return "kill-switch";
    case Reason::SwitchesOff: return "kmd-switches-off";
    case Reason::Hosted: return "hosted";
    }
    return "unknown";
}

// ---------------------------------------------------------------- applications

// AppRouter Mode: absent = Cpu; a REG_SZ other than the three names, or another value kind, = Invalid (CPU).
enum class AppMode { Cpu, Allowlist, GpuDefault, Invalid };
enum class AppRoute { Cpu, Gpu };

enum class AppReason {
    NoExe,        // the process image name could not be read
    ModeCpu,      // Mode absent or "cpu": the application kill switch
    ModeInvalid,  // Mode present but not one of the three names (fail safe: CPU)
    Protected,    // a logon or secure-desktop process (built-in list below), never routed to the GPU UMD
    RenderOnCpu,  // the per-application graphics setting RenderOnCpu is 1 (docs/design/per-app-graphics-settings.md)
    Denied,       // listed in Deny (both modes; Deny wins over Allow)
    DialogHost,   // gpu-default, on the router's built-in default Deny list (IsCpuOnlyDialogHost below), not in Allow
    NotAllowed,   // allowlist mode and not listed in Allow
    ComponentUnknown, // gpu-default mode, the image or the Windows directory could not be resolved, not in Allow
    D3d10Entry,   // OpenAdapter10: the application GPU UMD exports OpenAdapter10_2 only. No Windows 11 runtime calls
                  // this entry while OpenAdapter10_2 is exported, the D3D10.0 runtime included: a D3D10.0
                  // application arrives at OpenAdapter10_2 and is decided like any other (BD-081, measured
                  // 2026-10-07, evidence/windows/2026-10-07-BD081-d3d10-entry-devpc)
    GpuUmdUnset,  // GpuUmdPath absent, not a REG_SZ or not an absolute path
    Allowed,      // allowlist mode and listed in Allow: the GPU UMD
    Default,      // gpu-default mode and not denied: the GPU UMD
};

// Whether the process image is a Windows component (IsWindowsComponentPath on resolved paths, router-identity.h).
enum class Component { No, Yes, Unknown };

struct AppInputs {
    const wchar_t *exe_base;
    AppMode mode;
    const wchar_t *allow;  // REG_MULTI_SZ block or nullptr
    const wchar_t *deny;   // REG_MULTI_SZ block or nullptr
    bool entry_10_2;       // the call is OpenAdapter10_2 (D3D10.1/D3D11 runtimes)
    bool gpu_umd_set;      // GpuUmdPath is an absolute path
    Component component = Component::No; // ClassifyComponent (router-identity.h) of the process image
    bool render_on_cpu = false; // RenderOnCpu of HKLM\SOFTWARE\amdgpu-wddm\Graphics (app-settings.h) is 1
};

struct AppDecision {
    AppRoute route;
    AppReason reason;
};

// Processes of the logon and secure desktops. A GPU UMD fault there could lock the owner out of the session, so
// no registry value routes them to the application GPU UMD. (dwm.exe never reaches the application decision.)
inline bool IsProtectedApp(const wchar_t *exe_base)
{
    static const wchar_t list[] = L"logonui.exe\0consent.exe\0lockapp.exe\0credentialuibroker.exe\0winlogon.exe\0";
    return InList(exe_base, list);
}

// The router's built-in default Deny list: images kept on the CPU UMD without any registry entry, because the
// application GPU UMD was measured not to show their window. One image per measured failure, never a class of
// images: the 2026-10-04 mitigation that sent every Windows component to the CPU UMD cost those processes FL 12_1
// (BD-088). BD-061, the WinUI/DirectComposition defect behind that mitigation, is VERIFIED fixed since b20, so an
// entry here is a new failure of its own and not an instance of a known class.
//  - PickerHost.exe, the File Picker UI Host, hosts FirewallNotificationDialogServer and
//    FirewallWarningDialogServer, the Windows firewall notification dialogs, and on current builds the modern file
//    open and save dialogs as well. On 2026-10-12 (tester.28, gpu-default) the firewall notification showed an
//    empty white window with only the application icon and did not paint in two minutes, while the overlay read
//    "PickerHost.exe D3D11 GPU" (BD-126). This entry changes the route and nothing else: whether that dialog
//    paints on the CPU UMD is not measured yet, the CPU UMD offers FL 10_0 only (BD-088), so content that demands
//    FL 11_0 gets no device there, and every window of this image moves with it, the file dialogs included.
//  - SecHealthUI.exe, the Windows Security app. On 2026-10-20 (tester.31v2 on 26100, gpu-default) opening it made
//    DWM crash in a loop, about every 7 s (dwmcore.dll 0x8898008D), until its window closed; with the image in the
//    registry Deny list it opened and worked with no crash (BD-168 class). Users open it to turn Smart App Control
//    off. Like the entry above, this one routes around a failure that is not diagnosed yet.
// The function name and the reason app-dialog-host come from the first entry; the list holds any image measured to
// fail on the application GPU UMD, one image per failure.
// The route decision is taken at OpenAdapter10_2, before any device or window exists, so the process image name is
// all the router has to go by; the entry is therefore a base name, matched exactly and case-insensitively.
// An Allow entry names an image explicitly, which a default cannot, so Allow lifts this list: under gpu-default
// here, and under allowlist through the allowlist's own answer. The GPU route can be measured again (the open
// BD-126 question) without a new router build. A registry Deny entry, the protected list, RenderOnCpu and, under
// allowlist, "not listed in Allow" answer before it and keep their own reasons.
inline bool IsCpuOnlyDialogHost(const wchar_t *exe_base)
{
    static const wchar_t list[] = L"pickerhost.exe\0sechealthui.exe\0";
    return InList(exe_base, list);
}

// Windows components: images below the Windows directory (System32, SystemApps, ImmersiveControlPanel and the
// rest) and packaged apps below "<anything>\WindowsApps\Microsoft*". The second rule is a compatibility heuristic
// on the directory name, not a publisher check. Until train b20, gpu-default kept such images on the CPU UMD (FL
// 10_0, BD-088) because Notepad, Calculator and Task Manager failed on the application GPU UMD (BD-061,
// 2026-10-04). The shared A8/FP16/RGB10A2 surfaces of the D3D11 shell fixed that (verified on b20), so a component
// now takes the GPU UMD like any other image; only an image whose path cannot be resolved (Component::Unknown)
// stays on the CPU UMD. The router passes final resolved paths (router-identity.h), so "\\?\", 8.3 and junction
// spellings reach this test as one form.
// Prefix tests are case-insensitive and end at a path separator, so "C:\Windows2" or "C:\Windows.old" do not match.
inline bool HasDirPrefix(const wchar_t *path, const wchar_t *dir)
{
    if (!path || !dir || !*dir) return false;
    size_t n = wcslen(dir);
    while (n && (dir[n - 1] == L'\\' || dir[n - 1] == L'/')) --n;
    if (!n || _wcsnicmp(path, dir, n)) return false;
    return path[n] == L'\\' || path[n] == L'/';
}

inline bool IsWindowsComponentPath(const wchar_t *image, const wchar_t *windows_dir)
{
    if (!image || !*image) return false;
    if (HasDirPrefix(image, windows_dir)) return true;
    // "...\WindowsApps\Microsoft.<package>\..." or "...\WindowsApps\MicrosoftWindows.<package>\...".
    for (const wchar_t *p = image; *p; ++p) {
        if (*p != L'\\' && *p != L'/') continue;
        static const wchar_t apps[] = L"WindowsApps";
        const size_t n = sizeof(apps) / sizeof(apps[0]) - 1;
        if (!_wcsnicmp(p + 1, apps, n) && (p[1 + n] == L'\\' || p[1 + n] == L'/') &&
            !_wcsnicmp(p + 2 + n, L"Microsoft", 9))
            return true;
    }
    return false;
}

inline AppMode ParseAppMode(const wchar_t *text)
{
    if (!text) return AppMode::Invalid;
    if (!_wcsicmp(text, L"cpu")) return AppMode::Cpu;
    if (!_wcsicmp(text, L"allowlist")) return AppMode::Allowlist;
    if (!_wcsicmp(text, L"gpu-default")) return AppMode::GpuDefault;
    return AppMode::Invalid;
}

inline AppDecision DecideApp(const AppInputs &in)
{
    if (!in.exe_base || !*in.exe_base) return {AppRoute::Cpu, AppReason::NoExe};
    if (in.mode == AppMode::Cpu) return {AppRoute::Cpu, AppReason::ModeCpu};
    if (in.mode != AppMode::Allowlist && in.mode != AppMode::GpuDefault) return {AppRoute::Cpu, AppReason::ModeInvalid};
    if (IsProtectedApp(in.exe_base)) return {AppRoute::Cpu, AppReason::Protected};
    // The graphics setting adds to the AppRouter Deny list, which stays as it was: either one keeps the application
    // on the CPU UMD. A RenderOnCpu of 0 never moves an application off the CPU UMD that the policy chose.
    if (in.render_on_cpu) return {AppRoute::Cpu, AppReason::RenderOnCpu};
    if (InList(in.exe_base, in.deny)) return {AppRoute::Cpu, AppReason::Denied};
    if (in.mode == AppMode::Allowlist && !InList(in.exe_base, in.allow)) return {AppRoute::Cpu, AppReason::NotAllowed};
    // The built-in default Deny list (BD-126): a default, so an explicit Allow entry lifts it. It is tested after
    // the allowlist, which already keeps every unlisted image on the CPU UMD and keeps its own reason, so this
    // test answers under gpu-default only.
    if (IsCpuOnlyDialogHost(in.exe_base) && !InList(in.exe_base, in.allow))
        return {AppRoute::Cpu, AppReason::DialogHost};
    if (in.mode == AppMode::GpuDefault && in.component == Component::Unknown && !InList(in.exe_base, in.allow))
        return {AppRoute::Cpu, AppReason::ComponentUnknown};
    if (!in.entry_10_2) return {AppRoute::Cpu, AppReason::D3d10Entry};
    if (!in.gpu_umd_set) return {AppRoute::Cpu, AppReason::GpuUmdUnset};
    return {AppRoute::Gpu, in.mode == AppMode::Allowlist ? AppReason::Allowed : AppReason::Default};
}

inline const char *AppModeName(AppMode m)
{
    switch (m) {
    case AppMode::Cpu: return "cpu";
    case AppMode::Allowlist: return "allowlist";
    case AppMode::GpuDefault: return "gpu-default";
    case AppMode::Invalid: return "invalid";
    }
    return "unknown";
}

inline const char *AppReasonName(AppReason r)
{
    switch (r) {
    case AppReason::NoExe: return "app-no-exe";
    case AppReason::ModeCpu: return "app-mode-cpu";
    case AppReason::ModeInvalid: return "app-mode-invalid";
    case AppReason::Protected: return "app-protected";
    case AppReason::RenderOnCpu: return "app-render-on-cpu";
    case AppReason::Denied: return "app-denied";
    case AppReason::DialogHost: return "app-dialog-host";
    case AppReason::NotAllowed: return "app-not-allowed";
    case AppReason::ComponentUnknown: return "app-component-unknown";
    case AppReason::D3d10Entry: return "app-d3d10-entry";
    case AppReason::GpuUmdUnset: return "app-gpu-umd-unset";
    case AppReason::Allowed: return "app-allowed";
    case AppReason::Default: return "app-default";
    }
    return "unknown";
}

} // namespace bc250router
