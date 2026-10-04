// The KMD through bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c built with BC250_CONTROL_DLL). Every read
// here is a software-state escape with NoAdapterSynchronization alone: no HardwareAccess (Level Two) escape, which
// would idle the GPU and stall a running game (BD-054). The one write is ConfirmStart, the start-health CONFIRM the
// release's logon task sends as well: administrator only, HardwareAccess, once per Recovery action. Settings go to
// the registry and take effect at the next driver start.
using System;
using System.Runtime.InteropServices;

namespace AmdgpuWddmControl
{
    // A normal restart of Windows through ExitWindowsEx (EWX_REBOOT, planned, "application: reconfiguration"):
    // programs are asked to close and can keep their unsaved work. The window calls it only after the user confirms.
    public static class WindowsRestart
    {
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        struct TokenPrivilege { public uint Count; public long Luid; public uint Attributes; }

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool LookupPrivilegeValue(string system, string name, out long luid);
        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivilege state, uint length, IntPtr previous, IntPtr returnLength);
        [DllImport("user32.dll", SetLastError = true)]
        static extern bool ExitWindowsEx(uint flags, uint reason);
        [DllImport("kernel32.dll")]
        static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        const uint TokenAdjustPrivileges = 0x20, TokenQuery = 0x8, PrivilegeEnabled = 2;
        const uint EwxReboot = 0x2;
        const uint ReasonPlannedApplicationReconfig = 0x80000000u | 0x00040000u | 0x00000004u;

        // null when Windows accepted the restart, else the error.
        public static string Request()
        {
            IntPtr token;
            if (!OpenProcessToken(GetCurrentProcess(), TokenAdjustPrivileges | TokenQuery, out token)) return "OpenProcessToken error " + Marshal.GetLastWin32Error();
            try
            {
                var p = new TokenPrivilege { Count = 1, Attributes = PrivilegeEnabled };
                if (!LookupPrivilegeValue(null, "SeShutdownPrivilege", out p.Luid)) return "LookupPrivilegeValue error " + Marshal.GetLastWin32Error();
                if (!AdjustTokenPrivileges(token, false, ref p, 0, IntPtr.Zero, IntPtr.Zero)) return "AdjustTokenPrivileges error " + Marshal.GetLastWin32Error();
                int e = Marshal.GetLastWin32Error();
                if (e != 0) return "this account may not restart Windows (error " + e + ")";
            }
            finally { CloseHandle(token); }
            return ExitWindowsEx(EwxReboot, ReasonPlannedApplicationReconfig) ? null : "ExitWindowsEx error " + Marshal.GetLastWin32Error();
        }
    }

    public sealed class KmdResult<T> where T : class
    {
        public T Value;
        public int Status;          // NTSTATUS of the request, 0 on success
        public string Error;        // plain-language reason when Value is null
        public bool DriverMissing { get { return (uint)Status == 0xC000000E; } }
    }

    public static class Kmd
    {
        const string Dll = "bc250control.dll";
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Dpm([Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Interop([Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250StartHealth(uint op, ulong generation, ulong epoch, [Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi, CharSet = CharSet.Unicode)]
        static extern int Bc250VideoMemory(string hardwareId, [Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250LogRead(uint from, [Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250CuMode(uint op, ulong expectedGeneration, [Out] byte[] data, uint bytes);

        // AMDGPU_WDDM_CONTROL_NO_DLL=1: every driver read answers as if bc250control.dll were missing (the build's
        // check of the recovery view and of the pages without the DLL).
        // Blocked: the recovery view (--recovery), which must work when the DLL or the driver cannot.
        public static bool Blocked;
        static bool DllBlocked { get { return Blocked || Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_NO_DLL") == "1"; } }

        static KmdResult<T> Call<T>(int size, Func<byte[], int> request, Func<byte[], T> parse) where T : class
        {
            var result = new KmdResult<T>();
            var buffer = new byte[size];
            try
            {
                if (DllBlocked) throw new DllNotFoundException();
                result.Status = request(buffer);
                if (result.Status < 0) { result.Error = KmdReply.StatusText(result.Status); return result; }
                result.Value = parse(buffer);
            }
            catch (DllNotFoundException) { result.Status = unchecked((int)0xC0000135); result.Error = "bc250control.dll is missing next to the application"; }
            catch (EntryPointNotFoundException) { result.Status = unchecked((int)0xC0000139); result.Error = "bc250control.dll is too old for this application"; }
            catch (FormatException e) { result.Status = unchecked((int)0xC000000D); result.Error = "Unexpected driver reply: " + e.Message; }
            return result;
        }

        public static KmdResult<DpmState> Dpm()
        {
            return Call(KmdReply.DpmBytes, b => Bc250Dpm(b, (uint)b.Length), KmdReply.ParseDpm);
        }

        public static KmdResult<InteropState> Interop()
        {
            return Call(KmdReply.InteropBytes, b => Bc250Interop(b, (uint)b.Length), KmdReply.ParseInterop);
        }

        public static KmdResult<StartHealthState> StartHealth()
        {
            return Call(KmdReply.StartHealthBytes, b => Bc250StartHealth(0, 0, 0, b, (uint)b.Length), KmdReply.ParseStartHealth);
        }

        // BC250_START_HEALTH_CONFIRM with the generation and epoch of the reading it confirms; the KMD checks its own
        // milestone again and the DLL checks that the reply has CONFIRMED set for that start.
        public static KmdResult<StartHealthState> ConfirmStart(ulong generation, ulong epoch)
        {
            return Call(KmdReply.StartHealthBytes, b => Bc250StartHealth(1, generation, epoch, b, (uint)b.Length), KmdReply.ParseStartHealth);
        }

        // The CU mode snapshot of this start (READ, any caller).
        public static KmdResult<CuModeState> CuMode()
        {
            return Call(KmdReply.CuModeBytes, b => Bc250CuMode(0, 0, b, (uint)b.Length), KmdReply.ParseCuMode);
        }

        // BC250_CU_MODE_OP_CONFIRM with the Generation of a fresh READ of this start: administrator only. The KMD
        // deletes CuModePending first, then stores CuModeConfirmed; either step can fail, so the caller reads again.
        public static KmdResult<CuModeState> CuConfirm(ulong generation)
        {
            return Call(KmdReply.CuModeBytes, b => Bc250CuMode(1, generation, b, (uint)b.Length), KmdReply.ParseCuMode);
        }

        public static KmdResult<VideoMemoryState> VideoMemory()
        {
            return Call(KmdReply.VideoMemoryBytes, b => Bc250VideoMemory(null, b, (uint)b.Length), KmdReply.ParseVideoMemory);
        }

        public static KmdResult<LogPage> LogPage(uint from)
        {
            return Call(KmdReply.LogBytes, b => Bc250LogRead(from, b, (uint)b.Length), KmdReply.ParseLog);
        }

        // SYSTEM_CODEINTEGRITY_INFORMATION (class 103): CODEINTEGRITY_OPTION_TESTSIGN is 0x2. No administrator needed.
        [StructLayout(LayoutKind.Sequential)]
        struct CodeIntegrityInformation { public uint Length; public uint Options; }
        [DllImport("ntdll.dll")]
        static extern int NtQuerySystemInformation(int infoClass, ref CodeIntegrityInformation info, int length, out int returned);

        public static bool? TestSigning()
        {
            var info = new CodeIntegrityInformation { Length = 8 };
            int returned;
            try { return NtQuerySystemInformation(103, ref info, 8, out returned) >= 0 ? (info.Options & 2) != 0 : (bool?)null; }
            catch (Exception) { return null; }
        }
    }
}
