// Thin consumer of driver/state/client.h. Defaults and validation stay native.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Bc250Mon
{
    public sealed class DriverStateSpec
    {
        public string Path, Name;
        public uint Kind, Type, MaxBytes;
    }
    public sealed class DriverStateValue
    {
        public uint Status, Error, OsError, Type, Source;
        public object Data;
        public bool Absent { get { return Status == 1; } }
        public bool Ok { get { return Status == 0 && Error == 0; } }
        public string Failure { get { return "state status " + Status + ", Windows error " + Error; } }
        public object Require()
        {
            if (Absent) return null;
            if (!Ok) throw new Win32Exception(unchecked((int)(Error == 0 ? 13u : Error)), Failure);
            return Data;
        }
        public string SourceName { get { return Source == 1 ? "override" : Source == 2 ? "factory" : Source == 3 ? "code" : "none"; } }
    }
    public interface IDriverStateSession : IDisposable
    {
        IEnumerable<DriverStateSpec> Catalog { get; }
        DriverStateValue Read(string path, string name, bool effective);
        void WriteDword(string path, string name, uint value);
    }
    public static class DriverState
    {
        // Tests replace this boundary with an in-memory session. Production has
        // no path override and always resolves the installed service through SCM.
        internal static Func<bool, IDriverStateSession> OpenSession = NativeDriverState.Open;
        public static IDriverStateSession Open(bool writable = false) { return OpenSession(writable); }
    }
    sealed class NativeDriverState : IDriverStateSession
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        struct Spec
        {
            public uint Abi, Bytes, Kind, Type, MaxBytes, Access, PrivateRead, Group;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Path;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Name;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Value { public uint Status, OsError, Type, Bytes, Source; }
        [DllImport("bc250control.dll", ExactSpelling = true)] static extern uint AmdgpuClientCount();
        [DllImport("bc250control.dll", ExactSpelling = true)] static extern uint AmdgpuClientSpec(uint index, out Spec spec, uint bytes);
        [DllImport("bc250control.dll", ExactSpelling = true)] static extern uint AmdgpuClientOpen(out IntPtr state, uint writable);
        [DllImport("bc250control.dll", ExactSpelling = true)] static extern void AmdgpuClientClose(IntPtr state);
        [DllImport("bc250control.dll", ExactSpelling = true, CharSet = CharSet.Ansi)]
        static extern uint AmdgpuClientRead(IntPtr state, string path, string name, uint effective, out Value value,
            [Out] byte[] data, uint bytes);
        [DllImport("bc250control.dll", ExactSpelling = true, CharSet = CharSet.Ansi)]
        static extern uint AmdgpuClientWrite(IntPtr state, string path, string name, uint type, byte[] data, uint bytes);

        IntPtr handle;
        readonly List<DriverStateSpec> catalog = new List<DriverStateSpec>();
        public IEnumerable<DriverStateSpec> Catalog { get { return catalog; } }
        public static IDriverStateSession Open(bool writable)
        {
            IntPtr handle;
            Check(AmdgpuClientOpen(out handle, writable ? 1u : 0u), "open driver state");
            try { return new NativeDriverState(handle); }
            catch { AmdgpuClientClose(handle); throw; }
        }
        NativeDriverState(IntPtr state)
        {
            handle = state;
            uint count = AmdgpuClientCount();
            if (count > 4096) throw new InvalidOperationException("Invalid state catalog size");
            for (uint i = 0; i < count; ++i)
            {
                Spec spec; Check(AmdgpuClientSpec(i, out spec, (uint)Marshal.SizeOf(typeof(Spec))), "read state catalog");
                if (spec.Abi != 1 || spec.Bytes != Marshal.SizeOf(typeof(Spec)) || spec.MaxBytes > 262144)
                    throw new InvalidOperationException("Invalid state catalog ABI");
                catalog.Add(new DriverStateSpec { Path = spec.Path, Name = spec.Name, Kind = spec.Kind,
                    Type = spec.Type, MaxBytes = spec.MaxBytes });
            }
        }
        static void Check(uint error, string action)
        { if (error != 0) throw new Win32Exception(unchecked((int)error), action + ": Windows error " + error); }
        public DriverStateValue Read(string path, string name, bool effective)
        {
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("NativeDriverState");
            DriverStateSpec spec = catalog.Find(s => string.Equals(s.Path, path, StringComparison.OrdinalIgnoreCase)
                && string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
            if (spec == null) throw new ArgumentException("Unknown state value: " + path + "/" + name);
            var bytes = new byte[spec.MaxBytes]; Value value;
            uint error = AmdgpuClientRead(handle, spec.Path, spec.Name, effective ? 1u : 0u, out value, bytes, (uint)bytes.Length);
            var result = new DriverStateValue { Status = value.Status, Error = error, OsError = value.OsError,
                Type = value.Type, Source = value.Source };
            if (!result.Ok) return result;
            if (value.Type != spec.Type || value.Bytes > bytes.Length) throw new InvalidOperationException("Invalid state reply");
            switch (value.Type)
            {
                case 4: if (value.Bytes != 4) throw new InvalidOperationException("Invalid DWORD state reply");
                    result.Data = BitConverter.ToInt32(bytes, 0); break;
                case 11: if (value.Bytes != 8) throw new InvalidOperationException("Invalid QWORD state reply");
                    result.Data = BitConverter.ToInt64(bytes, 0); break;
                case 1: result.Data = Encoding.Unicode.GetString(bytes, 0, checked((int)value.Bytes) - 2); break;
                case 7: result.Data = Encoding.Unicode.GetString(bytes, 0, checked((int)value.Bytes)).TrimEnd('\0')
                    .Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries); break;
                case 3: var data = new byte[value.Bytes]; Array.Copy(bytes, data, value.Bytes); result.Data = data; break;
                default: throw new InvalidOperationException("Invalid state value type");
            }
            return result;
        }
        public void WriteDword(string path, string name, uint value)
        {
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("NativeDriverState");
            Check(AmdgpuClientWrite(handle, path, name, 4, BitConverter.GetBytes(value), 4), "persist " + name);
        }
        public void Dispose() { if (handle != IntPtr.Zero) { AmdgpuClientClose(handle); handle = IntPtr.Zero; } }
    }
}
