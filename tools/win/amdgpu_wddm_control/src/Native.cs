// The KMD through bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c built with BC250_CONTROL_DLL). Every read
// here is a software-state escape with NoAdapterSynchronization alone: no HardwareAccess (Level Two) escape, which
// would idle the GPU and stall a running game (BD-054). The one write is ConfirmStart, the start-health CONFIRM the
// release's logon task sends as well: administrator only, once per Recovery action, and from 0.7.213 with
// NoAdapterSynchronization instead of HardwareAccess - it writes the registry and touches no register. A driver of
// 0.7.212 or older refuses that word, which is what runs while a release defers the device restart, so the DLL sends
// the same request once more with the old HardwareAccess word rather than leave the start unconfirmed: one Level Two
// escape per Recovery action, never on a schedule. Settings go to
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
        static extern int Bc250BoardMemory([Out] byte[] reply, uint bytes, [In] byte[] request);
        public static KmdResult<UmaState> BoardMemoryQuery() { return BoardMemory(0, 0, null); }
        public static KmdResult<UmaState> BoardMemory(uint operation = 0, uint target = 0, UmaState expected = null)
        {
            var request = UmaSetting.Request(operation, target, expected);
            return Call(128, b => Bc250BoardMemory(b, (uint)b.Length, request), b => UmaSetting.Parse(b, 32));
        }
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
        // Added to bc250control.dll with KMD 0.7.213.1. A deployed DLL without it throws
        // EntryPointNotFoundException, which Call() turns into "bc250control.dll is too old for this application".
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Hwmon([Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250DpmCurve(uint op, ulong expectedGeneration, uint[] mv, uint points, uint windowMs, [Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Cpu(ref CpuRequest request, [Out] byte[] data, uint bytes);

        // BC250_CPU_REQUEST of tools/win/bc250kmd_cli/bc250kmd_cli.c, 56 bytes. Size says which fields the DLL may
        // read, so an older application and a newer DLL refuse each other instead of guessing.
        // WheaEvents, ChecksumErrors and Loaded are what the caller knows and the driver cannot see: this app
        // runs no load of its own, so it leaves all three at 0 and the driver judges no clock stretching.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        public struct CpuRequest
        {
            public uint Size, Op, Given, MaxMHz, UvSteps, TempC, TrialMs, CoreMask;
            public uint WheaEvents, ChecksumErrors, Loaded;
            public ulong ExpectedGeneration;
        }

        // Added with the case fan control (RUN_FAN). An older DLL throws EntryPointNotFoundException (the card then says
        // the application and the driver do not match); an older driver answers 0xC00000BB.
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Fan(ref FanRequest request, [Out] byte[] data, uint bytes);

        // BC250_FAN_REQUEST of tools/win/bc250kmd_cli/bc250kmd_cli.c, 104 bytes. Size says which fields the DLL may read.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        public struct FanRequest
        {
            public uint Size, Op, Profile, Points;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] CurveC;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] CurvePct;
            public uint FixedPct, LeaseMs, Store, Reserved;
            public ulong ExpectedGeneration;
        }

        public const uint FanOpRead = 0, FanOpBoard = 1, FanOpCurve = 2, FanOpFixed = 3, FanOpRenew = 4;

        public const uint CurveOpRead = 0, CurveOpSet = 1, CurveOpKeep = 2, CurveOpCancel = 3, CurveOpReset = 4;
        public const uint CpuOpRead = 0, CpuOpReadback = 1, CpuOpSet = 2, CpuOpKeep = 3, CpuOpCancel = 4,
            CpuOpReset = 5, CpuOpCores = 6, CpuOpSearchBegin = 7, CpuOpSearchStep = 8;

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

        // RUN_DPM with ABI 3 (KMD 0.7.215, the SMU metrics tail with the power reading), else ABI 1. A driver or a
        // bc250control.dll from before 0.7.215 refuses the 248-byte request with STATUS_INVALID_PARAMETER before
        // anything is read; from then on this process asks with ABI 1 alone, so an old driver costs one refused
        // request per run and not one per poll.
        static volatile bool _dpmAbi3Refused;

        public static KmdResult<DpmState> Dpm()
        {
            if (!_dpmAbi3Refused)
            {
                var r = Call(KmdReply.DpmAbi3Bytes, b => Bc250Dpm(b, (uint)b.Length), KmdReply.ParseDpm);
                bool refused = r.Value == null && (uint)r.Status == 0xC000000D && r.Error == KmdReply.StatusText(r.Status);
                if (!refused) return r;
                _dpmAbi3Refused = true;
            }
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

        // The board's hardware monitor: the fan speed and the chip's own temperatures (READ, any caller). The
        // reading needs EnableHwmon open on the lab machine; with the gate closed the reply says so in its flags.
        public static KmdResult<HwmonState> Hwmon()
        {
            return Call(KmdReply.HwmonBytes, b => Bc250Hwmon(b, (uint)b.Length), KmdReply.ParseHwmon);
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

        // The V/F curve. Every operation is a software escape; a write needs an administrator and the Generation of
        // a read of the same start, so the window reads, then acts on what it read.
        public static KmdResult<CurveState> Curve()
        {
            return Call(KmdReply.CurveBytes, b => Bc250DpmCurve(CurveOpRead, 0, null, 0, 0, b, (uint)b.Length), KmdReply.ParseCurve);
        }

        // A trial of one curve. The driver reverts it when the window passes without a Keep, so a window that
        // closes, a crash and a power cut all end the same way: the stored curve comes back.
        public static KmdResult<CurveState> CurveTrial(ulong generation, uint[] mv, uint windowMs)
        {
            var values = mv == null ? new uint[KmdReply.CurvePoints] : (uint[])mv.Clone();
            return Call(KmdReply.CurveBytes, b => Bc250DpmCurve(CurveOpSet, generation, values, (uint)values.Length, windowMs, b, (uint)b.Length), KmdReply.ParseCurve);
        }

        public static KmdResult<CurveState> CurveOp(uint op, ulong generation)
        {
            return Call(KmdReply.CurveBytes, b => Bc250DpmCurve(op, generation, null, 0, 0, b, (uint)b.Length), KmdReply.ParseCurve);
        }

        // The processor surface. READ is a software snapshot, and so is KEEP from 0.7.213 (it writes the registry
        // and ends the trial); READBACK and every other write send mailbox messages, so the DLL sends those with
        // HardwareAccess and the driver takes the adapter for the sequence.
        public static KmdResult<CpuState> Cpu()
        {
            var r = new CpuRequest { Size = (uint)Marshal.SizeOf(typeof(CpuRequest)), Op = CpuOpRead };
            return Call(KmdReply.CpuBytes, b => Bc250Cpu(ref r, b, (uint)b.Length), KmdReply.ParseCpu);
        }

        public static KmdResult<CpuState> CpuRequestOp(CpuRequest request)
        {
            request.Size = (uint)Marshal.SizeOf(typeof(CpuRequest));
            var r = request;
            return Call(KmdReply.CpuBytes, b => Bc250Cpu(ref r, b, (uint)b.Length), KmdReply.ParseCpu);
        }

        // The case fan control. READ is open to every caller; a write needs an administrator and the Generation of a
        // READ of the same start. Every operation is a software escape: the driver applies a write at its next step.
        public static KmdResult<FanState> Fan()
        {
            var r = NewFanRequest(FanOpRead);
            return Call(KmdReply.FanBytes, b => Bc250Fan(ref r, b, (uint)b.Length), KmdReply.ParseFan);
        }

        public static FanRequest NewFanRequest(uint op)
        {
            return new FanRequest
            {
                Size = (uint)Marshal.SizeOf(typeof(FanRequest)), Op = op,
                CurveC = new uint[KmdReply.FanCurveSlots], CurvePct = new uint[KmdReply.FanCurveSlots],
            };
        }

        public static KmdResult<FanState> FanRequestOp(FanRequest request)
        {
            request.Size = (uint)Marshal.SizeOf(typeof(FanRequest));
            var r = request;
            return Call(KmdReply.FanBytes, b => Bc250Fan(ref r, b, (uint)b.Length), KmdReply.ParseFan);
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
