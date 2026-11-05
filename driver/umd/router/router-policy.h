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
    Protected,    // winlogon.exe (never a GPU UMD), or a gated logon image with AppRouter ProtectedGpu set to 0
    ProtectedDesktopCpu,  // a gated logon image, and the desktop does not take the GPU route (ProtectedGates below)
    ProtectedUnconfirmed, // a gated logon image, and the kernel driver has not confirmed this start
    ProtectedFaulted,     // a gated logon image whose GPU process of this boot ended without a clean exit
    ProtectedReset,       // a gated logon image, and the adapter was reset after the latch base of this boot
    ProtectedUnlatched,   // LogonUI or consent, and the fault latch cannot be read or armed
    RenderOnCpu,  // the per-application graphics setting RenderOnCpu is 1 (docs/design/per-app-graphics-settings.md)
    Denied,       // listed in Deny (both modes; Deny wins over Allow)
    DialogHost,   // gpu-default, on the router's built-in default Deny list (IsCpuOnlyDialogHost below), not in Allow
    NotAllowed,   // allowlist mode and not listed in Allow
    D3d10Entry,   // OpenAdapter10: the application GPU UMD exports OpenAdapter10_2 only. No Windows 11 runtime calls
                  // this entry while OpenAdapter10_2 is exported, the D3D10.0 runtime included: a D3D10.0
                  // application arrives at OpenAdapter10_2 and is decided like any other (BD-081, measured
                  // 2026-10-07, evidence/windows/2026-10-07-BD081-d3d10-entry-devpc)
    GpuUmdUnset,  // GpuUmdPath absent, not a REG_SZ or not an absolute path
    Allowed,      // allowlist mode and listed in Allow: the GPU UMD
    Default,      // gpu-default mode and not denied: the GPU UMD
};

// Whether the process image is a Windows component (IsWindowsComponentPath on resolved paths, router-identity.h).
// The route line reports it (component=yes|no|unknown); the decision does not depend on it (BD-088, see below).
enum class Component { No, Yes, Unknown };

// The start gate of the logon images (StartHealthOf below).
enum class StartHealth { Confirmed, Unconfirmed, Unreadable };

// The fault latch of one logon image in this boot (LatchOf below; router-latch.h reads and arms it).
enum class Latch { Clear, Faulted, Reset, Unavailable };

// What the latch store holds for one image, as router-latch.h read it.
struct LatchView {
    bool store;               // the latch key is open for writing (volatile, HKLM or else HKCU)
    bool faulted;             // the image's value is 1: a process of it ended without a clean exit in this boot
    unsigned dead_markers;    // markers of the image whose process is gone and never removed its marker
    bool restarts_known;      // HangRecovery\AdapterRestarts could be read (absent counts as 0)
    unsigned long restarts;
    bool base_known;          // the store holds the base of this boot
    unsigned long base;       // AdapterRestarts at the first decision of a logon image in this boot
};

// A marker that its process never removed is a process that did not end through ExitProcess: an unhandled exception
// (the default handler ends the process with TerminateProcess), a kill or a crash inside the GPU UMD. Any adapter
// restart after the base of the boot is a recovered TDR that may have come from a logon image; the latch does not
// try to name the process that caused it.
inline Latch LatchOf(const LatchView &v)
{
    if (!v.store) return Latch::Unavailable;
    if (v.faulted || v.dead_markers) return Latch::Faulted;
    if (!v.restarts_known || !v.base_known) return Latch::Unavailable;
    if (v.restarts != v.base) return Latch::Reset;
    return Latch::Clear;
}

struct AppInputs {
    const wchar_t *exe_base;
    AppMode mode;
    const wchar_t *allow;  // REG_MULTI_SZ block or nullptr
    const wchar_t *deny;   // REG_MULTI_SZ block or nullptr
    bool entry_10_2;       // the call is OpenAdapter10_2 (D3D10.1/D3D11 runtimes)
    bool gpu_umd_set;      // GpuUmdPath is an absolute path
    // ClassifyComponent (router-identity.h) of the process image. Reported on the route line only: since 2026-11-02
    // (BD-088) no value of it changes the decision, Unknown included.
    Component component = Component::No;
    bool render_on_cpu = false; // RenderOnCpu of HKLM\SOFTWARE\amdgpu-wddm\Graphics (app-settings.h) is 1
    // The gates of the logon images (ProtectedKindOf below). The defaults are the fail-safe answers, so that a caller
    // that does not fill them keeps every logon image on the CPU UMD.
    bool protected_gpu = false;          // AppRouter ProtectedGpu: absent or 1 = true; 0 or another kind = false
    bool desktop_gpu = false;            // the desktop decision for dwm.exe is the hosted GPU UMD (DesktopAdmitsGpu)
    StartHealth start = StartHealth::Unreadable;
    Latch latch = Latch::Unavailable;
};

struct AppDecision {
    AppRoute route;
    AppReason reason;
};

// Processes of the logon and secure desktops (dwm.exe never reaches the application decision).
//   Never: winlogon.exe. It opens no Direct3D adapter (it starts LogonUI.exe and dwm.exe and draws nothing itself),
//          so the GPU UMD gives it nothing, and it stays on the CPU UMD as before.
//   Gated, latch required: LogonUI.exe (the sign-in and unlock credential screen) and consent.exe (the UAC prompt).
//          Both run as Local System on the secure desktop. A fault there stops the sign-in or every elevation, the
//          elevation that uninstalls this driver included. Both can write the fault latch in HKLM.
//   Gated, latch where it can be armed: LockApp.exe (the lock curtain, an AppContainer process of the user) and
//          CredentialUIBroker.exe (credential prompts inside the user's session). Their failure does not stop a
//          sign-in: a lock without the curtain still shows LogonUI.exe, and a failed prompt fails that one request.
//          LockApp.exe cannot write HKLM or HKCU, so for it the latch is best effort.
// A gated image takes the application GPU UMD only on a start that DWM itself would take to the GPU, that the kernel
// driver confirmed, and whose fault latch is clear (docs/design/logon-gpu-route.md). Otherwise the CPU UMD with a
// reason that names the gate. After the gates, the image follows the AppRouter policy like any other image.
enum class ProtectedKind { None, Never, GatedLatchRequired, GatedLatchBestEffort };

inline ProtectedKind ProtectedKindOf(const wchar_t *exe_base)
{
    if (InList(exe_base, L"winlogon.exe\0")) return ProtectedKind::Never;
    if (InList(exe_base, L"logonui.exe\0consent.exe\0")) return ProtectedKind::GatedLatchRequired;
    if (InList(exe_base, L"lockapp.exe\0credentialuibroker.exe\0")) return ProtectedKind::GatedLatchBestEffort;
    return ProtectedKind::None;
}

inline bool IsProtectedApp(const wchar_t *exe_base) { return ProtectedKindOf(exe_base) != ProtectedKind::None; }

// The desktop gate: the decision that Decide() gives dwm.exe with the same DesktopRouter key and kernel driver state
// (kill switch DwmForceCpu, the two start-latched switches), and the session's desktop-route record, when one can be
// read, does not say that the compositor ended on the CPU UMD (a failed hosted open, fallback=1). An unreadable or
// absent record does not close the gate: the registry conditions are the same ones that admitted DWM.
enum class DesktopRecord { Gpu, Cpu, Unknown };

inline bool DesktopAdmitsGpu(const Inputs &desktop, DesktopRecord record)
{
    Inputs dwm = desktop;
    dwm.exe_base = L"dwm.exe";
    return Decide(dwm).route == Route::Hosted && record != DesktopRecord::Cpu;
}

// The start gate, from two values of the kernel driver's state that every process can read
// (driver/kmd/guard.c): Parameters UnconfirmedStarts (incremented at every StartDevice, written 0 by the start
// confirmation, docs/design/wddm-start-confirmation.md) and the volatile GuardBoot\Confirmed (1 after a durable
// confirmation in this boot). Confirmed needs both: 0 unconfirmed starts and a confirmation in this boot. A runtime
// restart of the device in a confirmed boot counts one start again, so it is unconfirmed until it is confirmed.
inline StartHealth StartHealthOf(bool unconfirmed_known, unsigned long unconfirmed, bool boot_known,
                                 unsigned long boot_confirmed)
{
    if (!unconfirmed_known) return StartHealth::Unreadable;
    if (unconfirmed != 0 || !boot_known || boot_confirmed != 1) return StartHealth::Unconfirmed;
    return StartHealth::Confirmed;
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
//  SecHealthUI.exe, the Windows Security app, was on this list from tester.31v3 to tester.35: on 2026-10-20 (tester.31v2
//  on 26100) opening it on the GPU UMD made DWM crash in a loop (dwmcore.dll 0x8898008D, the BD-168 class). It is
//  off the list since 2026-11-02: with the BD-168 fix in the desktop zink (1BD064AD), the app opened on the GPU
//  route and painted its window, and DWM kept the same process (lab run 20261102T041155Z, BD-088 kit).
//  - SearchHost.exe, the Start menu search. On 2026-10-28 a tester (GitHub #44, tester.32 on 26300, 1080p59 over a
//    DP-to-HDMI adapter) reported that typing in the search made DWM crash every 3 to 7 s (48 times in 8 minutes,
//    dwmcore.dll 0x8898008D, the BD-168 class). The A/B of that tester: with the image in the registry Deny list,
//    no crash; with the entry removed, 8 crashes in 2 minutes. This entry also routes around a failure that is not
//    diagnosed yet; the search panel then draws on the CPU UMD.
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
    static const wchar_t list[] = L"pickerhost.exe\0searchhost.exe\0";
    return InList(exe_base, list);
}

// Windows components: images below the Windows directory (System32, SystemApps, ImmersiveControlPanel and the
// rest) and packaged apps below "<anything>\WindowsApps\Microsoft*". The second rule is a compatibility heuristic
// on the directory name, not a publisher check. Until train b20, gpu-default kept such images on the CPU UMD (FL
// 10_0, BD-088) because Notepad, Calculator and Task Manager failed on the application GPU UMD (BD-061,
// 2026-10-04). The shared A8/FP16/RGB10A2 surfaces of the D3D11 shell fixed that (verified on b20), so a component
// now takes the GPU UMD like any other image. Until 2026-11-02 an image whose path could not be resolved
// (Component::Unknown) stayed on the CPU UMD under gpu-default (reason app-component-unknown). Since that day (BD-088)
// the CPU route is empty: such an image also takes the GPU UMD, and only the user's own Deny and
// RenderOnCpu entries, the protected list and the built-in default Deny list keep an image on the CPU UMD. The router
// still classifies the image for its route line. It passes final resolved paths (router-identity.h), so "\\?\", 8.3
// and junction spellings reach this test as one form.
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
    switch (ProtectedKindOf(in.exe_base)) {
    case ProtectedKind::None: break;
    case ProtectedKind::Never: return {AppRoute::Cpu, AppReason::Protected};
    case ProtectedKind::GatedLatchRequired:
    case ProtectedKind::GatedLatchBestEffort:
        if (!in.protected_gpu) return {AppRoute::Cpu, AppReason::Protected};
        if (!in.desktop_gpu) return {AppRoute::Cpu, AppReason::ProtectedDesktopCpu};
        if (in.start != StartHealth::Confirmed) return {AppRoute::Cpu, AppReason::ProtectedUnconfirmed};
        if (in.latch == Latch::Faulted) return {AppRoute::Cpu, AppReason::ProtectedFaulted};
        if (in.latch == Latch::Reset) return {AppRoute::Cpu, AppReason::ProtectedReset};
        if (in.latch == Latch::Unavailable && ProtectedKindOf(in.exe_base) == ProtectedKind::GatedLatchRequired)
            return {AppRoute::Cpu, AppReason::ProtectedUnlatched};
        break; // past the gates: the policy below, as for any image
    }
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
    if (!in.entry_10_2) return {AppRoute::Cpu, AppReason::D3d10Entry};
    if (!in.gpu_umd_set) return {AppRoute::Cpu, AppReason::GpuUmdUnset};
    return {AppRoute::Gpu, in.mode == AppMode::Allowlist ? AppReason::Allowed : AppReason::Default};
}

inline const char *ComponentName(Component c)
{
    switch (c) {
    case Component::No: return "no";
    case Component::Yes: return "yes";
    case Component::Unknown: return "unknown";
    }
    return "invalid";
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
    case AppReason::ProtectedDesktopCpu: return "app-protected-desktop-cpu";
    case AppReason::ProtectedUnconfirmed: return "app-protected-unconfirmed";
    case AppReason::ProtectedFaulted: return "app-protected-faulted";
    case AppReason::ProtectedReset: return "app-protected-reset";
    case AppReason::ProtectedUnlatched: return "app-protected-unlatched";
    case AppReason::RenderOnCpu: return "app-render-on-cpu";
    case AppReason::Denied: return "app-denied";
    case AppReason::DialogHost: return "app-dialog-host";
    case AppReason::NotAllowed: return "app-not-allowed";
    case AppReason::D3d10Entry: return "app-d3d10-entry";
    case AppReason::GpuUmdUnset: return "app-gpu-umd-unset";
    case AppReason::Allowed: return "app-allowed";
    case AppReason::Default: return "app-default";
    }
    return "unknown";
}

inline const char *StartHealthName(StartHealth s)
{
    switch (s) {
    case StartHealth::Confirmed: return "confirmed";
    case StartHealth::Unconfirmed: return "unconfirmed";
    case StartHealth::Unreadable: return "unreadable";
    }
    return "invalid";
}

inline const char *LatchName(Latch l)
{
    switch (l) {
    case Latch::Clear: return "clear";
    case Latch::Faulted: return "faulted";
    case Latch::Reset: return "reset";
    case Latch::Unavailable: return "unavailable";
    }
    return "invalid";
}

} // namespace bc250router
