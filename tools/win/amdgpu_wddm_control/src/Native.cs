// The KMD through bc250control.dll (tools/win/bc250kmd_cli/bc250kmd_cli.c built with BC250_CONTROL_DLL). Every call
// here is a software-state escape with NoAdapterSynchronization alone: no HardwareAccess (Level Two) escape, which
// would idle the GPU and stall a running game (BD-054). The app never writes to the KMD through an escape; settings
// go to the registry and take effect at the next driver start.
using System;
using System.Runtime.InteropServices;

namespace AmdgpuWddmControl
{
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

        static KmdResult<T> Call<T>(int size, Func<byte[], int> request, Func<byte[], T> parse) where T : class
        {
            var result = new KmdResult<T>();
            var buffer = new byte[size];
            try
            {
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
