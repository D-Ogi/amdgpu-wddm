// The few Windows calls of the setup window: a planned restart (R3, C17: ExitWindowsEx with a planned reason, never
// EWX_FORCE, so programs are asked to close and may keep unsaved work), the elevation check and relaunch, and the
// control app's display preferences (HKCU\Software\amdgpu-wddm\Control, docs/gui/interfaces.md section 5), read only.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32;

namespace AmdgpuWddmSetup
{
    public static class Native
    {
        const uint EWX_REBOOT = 0x00000002;
        // SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_RECONFIG, as common.ps1's
        // Request-PlannedRestart passes it.
        const uint PlannedReason = 0x80000000 | 0x00040000 | 0x00000004;
        const uint TOKEN_ADJUST_PRIVILEGES = 0x20, TOKEN_QUERY = 0x8, SE_PRIVILEGE_ENABLED = 0x2;

        [StructLayout(LayoutKind.Sequential)]
        struct TokenPrivileges { public uint Count; public long Luid; public uint Attributes; }

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool LookupPrivilegeValue(string system, string name, out long luid);

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state, uint length, IntPtr previous, IntPtr returnLength);

        [DllImport("kernel32.dll")]
        static extern IntPtr GetCurrentProcess();

        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        [DllImport("user32.dll", SetLastError = true)]
        static extern bool ExitWindowsEx(uint flags, uint reason);

        // null when Windows accepted the request, else the reason (for the support file; the window says "restart
        // the computer yourself").
        public static string RequestPlannedRestart()
        {
            IntPtr token;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out token)) return "OpenProcessToken: " + new Win32Exception().Message;
            try
            {
                var tp = new TokenPrivileges { Count = 1, Attributes = SE_PRIVILEGE_ENABLED };
                if (!LookupPrivilegeValue(null, "SeShutdownPrivilege", out tp.Luid)) return "LookupPrivilegeValue: " + new Win32Exception().Message;
                if (!AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) return "AdjustTokenPrivileges: " + new Win32Exception().Message;
                int e = Marshal.GetLastWin32Error();
                if (e != 0) return "this account may not restart Windows (error " + e + ")";
            }
            finally { CloseHandle(token); }
            return ExitWindowsEx(EWX_REBOOT, PlannedReason) ? null : "ExitWindowsEx: " + new Win32Exception().Message;
        }

        public static bool IsElevated()
        {
            using (var id = WindowsIdentity.GetCurrent())
                return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
        }

        // Starts this program again as administrator with the same arguments (one UAC prompt). False when the user
        // declined the prompt.
        public static bool RelaunchElevated(string[] args)
        {
            var psi = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, CommandLine.Join(args)) { UseShellExecute = true, Verb = "runas" };
            try { using (Process.Start(psi)) return true; }
            catch (Win32Exception) { return false; }
        }

        const string ControlKey = @"Software\amdgpu-wddm\Control";

        // "Show Nagi" of the control app (unchecked = no value = false). Setup never writes it.
        public static bool ShowNagi()
        {
            try { using (var k = Registry.CurrentUser.OpenSubKey(ControlKey)) return k != null && k.GetValue("ShowNagi") is int && (int)k.GetValue("ShowNagi") == 1; }
            catch (Exception) { return false; }
        }

        // The language chosen in the control app, if any; else null (setup then uses Windows' language).
        public static string ChosenLanguage()
        {
            try { using (var k = Registry.CurrentUser.OpenSubKey(ControlKey)) { var v = k == null ? null : k.GetValue("Language") as string; return Array.IndexOf(Strings.Languages, v) >= 0 ? v : null; } }
            catch (Exception) { return null; }
        }
    }
}
