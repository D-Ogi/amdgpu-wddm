// Service state through the common native resolver. This class performs no GPU
// request, including when PnP reports Code 43. Raw capture and effective reads
// are separate so an absent override survives backup and reset.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class DriverStateKey : IDisposable
    {
        const string Dll = "bc250control.dll";
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        struct Spec
        {
            public uint Abi, Bytes, Kind, Type, MaxBytes, Access, PrivateRead, Group;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Path;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Name;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Value { public uint Status, Error, Type, Bytes, Source; }
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientCount();
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientSpec(uint index, out Spec value, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientOpen(out IntPtr state, uint write);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern void AmdgpuClientClose(IntPtr state);
        [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientRead(IntPtr state, string path, string name, uint effective,
            out Value value, [Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientWrite(IntPtr state, string path, string name, uint type, byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientClear(IntPtr state, string path, string name);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        internal static extern uint AmdgpuClientBeginBatch(out IntPtr batch);
        [DllImport(Dll, ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        internal static extern uint AmdgpuClientEndBatch(IntPtr batch);
        [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientBatchRead(IntPtr batch, string path, string name, uint effective,
            out Value value, [Out] byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientBatchWrite(IntPtr batch, string path, string name, uint type, byte[] data, uint bytes);
        [DllImport(Dll, ExactSpelling = true, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
        static extern uint AmdgpuClientBatchClear(IntPtr batch, string path, string name);

        IntPtr handle;
        readonly string child;
        readonly bool effective;
        readonly DriverStateBatch batch;
        readonly Dictionary<string, Spec> specs = new Dictionary<string, Spec>(StringComparer.OrdinalIgnoreCase);

        public static bool IsPath(string path)
        {
            return path != null && (string.Equals(path, DpmSettings.RegistryPath, StringComparison.OrdinalIgnoreCase) ||
                path.StartsWith(DpmSettings.RegistryPath + "\\", StringComparison.OrdinalIgnoreCase));
        }
        // A value that goes through the state client: every value of the driver's keys except the one that the app
        // reads and writes raw because it is not in the catalog (HardwarePointerSetting.cs).
        public static bool IsStateValue(string path, string name)
        {
            return IsPath(path) && !HardwarePointerSetting.Allowed(path, name);
        }
        public static DriverStateKey OpenPath(string path, bool writable = false, bool effective = true)
        {
            if (!IsPath(path)) throw new ArgumentException("Not a driver state identifier", "path");
            string child = path.Length == DpmSettings.RegistryPath.Length ? "" : path.Substring(DpmSettings.RegistryPath.Length + 1);
            return Open(child, writable, effective);
        }
        public static DriverStateKey Open(string child = "", bool writable = false, bool effective = true)
        {
            IntPtr handle;
            uint error = AmdgpuClientOpen(out handle, writable ? 1u : 0u);
            if (error == 1060) return null; // SCM: service does not exist, not a damaged/missing generation.
            Check(error, "open driver state");
            try { return new DriverStateKey(handle, child, effective); }
            catch { AmdgpuClientClose(handle); throw; }
        }
        internal static DriverStateKey OpenBatch(DriverStateBatch batch, string child, bool effective)
        { return new DriverStateKey(batch.Handle, child, effective, batch); }
        DriverStateKey(IntPtr state, string child, bool effective, DriverStateBatch batch = null)
        {
            handle = state; this.child = child; this.effective = effective; this.batch = batch;
            uint count = AmdgpuClientCount();
            if (count > 4096) throw new InvalidOperationException("Invalid driver state catalog size");
            for (uint i = 0; i < count; i++)
            {
                Spec spec;
                Check(AmdgpuClientSpec(i, out spec, (uint)Marshal.SizeOf(typeof(Spec))), "read state catalog");
                if (spec.Abi != 1 || spec.Bytes != Marshal.SizeOf(typeof(Spec)) || spec.MaxBytes > 262144)
                    throw new InvalidOperationException("Invalid driver state catalog ABI");
                if (string.Equals(spec.Path, child, StringComparison.OrdinalIgnoreCase)) specs.Add(spec.Name, spec);
            }
        }
        internal static void Check(uint error, string action)
        {
            if (error != 0) throw new Win32Exception(unchecked((int)error), action + ": Windows error " + error);
        }
        public string[] GetValueNames() { return specs.Keys.OrderBy(x => x, StringComparer.Ordinal).ToArray(); }
        public RegistryValueKind GetValueKind(string name)
        {
            Spec spec;
            if (!specs.TryGetValue(name, out spec)) throw new ArgumentException("Unknown driver state value: " + name);
            return (RegistryValueKind)spec.Type;
        }
        public int Query(string name, out uint type, out byte[] bytes)
        { return QueryCore(name, effective, out type, out bytes); }
        int QueryCore(string name, bool useEffective, out uint type, out byte[] bytes)
        {
            type = 0; bytes = null;
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("DriverStateKey");
            Spec spec;
            if (!specs.TryGetValue(name, out spec)) return 87;
            var buffer = new byte[spec.MaxBytes]; Value value;
            uint error = batch == null
                ? AmdgpuClientRead(handle, child, name, useEffective ? 1u : 0u, out value, buffer, (uint)buffer.Length)
                : AmdgpuClientBatchRead(batch.Handle, child, name, useEffective ? 1u : 0u, out value, buffer, (uint)buffer.Length);
            if (value.Status == 1 && error == 2) return 2;
            if (error != 0) return error == 2 ? 13 : unchecked((int)error);
            if (value.Status != 0 || value.Type != spec.Type || value.Bytes > buffer.Length) return 13;
            type = value.Type; bytes = new byte[value.Bytes]; Array.Copy(buffer, bytes, value.Bytes); return 0;
        }
        public object GetValue(string name, object absent = null, RegistryValueOptions options = RegistryValueOptions.None)
        { return ReadValue(name, absent, effective); }
        public object GetOverride(string name) { return ReadValue(name, null, false); }
        object ReadValue(string name, object absent, bool useEffective)
        {
            uint type; byte[] bytes; int error = QueryCore(name, useEffective, out type, out bytes);
            if (error == 2) return absent;
            Check(unchecked((uint)error), "read " + name);
            switch (type)
            {
                case 4: return BitConverter.ToInt32(bytes, 0);
                case 11: return BitConverter.ToInt64(bytes, 0);
                case 1: return Encoding.Unicode.GetString(bytes, 0, bytes.Length - 2);
                case 7: return Encoding.Unicode.GetString(bytes).TrimEnd('\0').Split(new[] { '\0' }, StringSplitOptions.RemoveEmptyEntries);
                case 3: return bytes;
                default: throw new InvalidOperationException("Invalid driver state type");
            }
        }
        uint Write(string name, uint type, byte[] bytes)
        {
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("DriverStateKey");
            return batch == null ? AmdgpuClientWrite(handle, child, name, type, bytes, (uint)bytes.Length)
                : AmdgpuClientBatchWrite(batch.Handle, child, name, type, bytes, (uint)bytes.Length);
        }
        public int SetDword(string name, uint value) { return unchecked((int)Write(name, 4, BitConverter.GetBytes(value))); }
        public int Clear(string name)
        {
            if (handle == IntPtr.Zero) throw new ObjectDisposedException("DriverStateKey");
            return unchecked((int)(batch == null ? AmdgpuClientClear(handle, child, name) : AmdgpuClientBatchClear(batch.Handle, child, name)));
        }
        public void SetValue(string name, object value, RegistryValueKind kind)
        {
            byte[] bytes;
            switch (kind)
            {
                case RegistryValueKind.DWord: bytes = BitConverter.GetBytes((int)value); break;
                case RegistryValueKind.QWord: bytes = BitConverter.GetBytes((long)value); break;
                case RegistryValueKind.String: bytes = Encoding.Unicode.GetBytes((string)value + "\0"); break;
                default: throw new InvalidOperationException("Unsupported driver state write type");
            }
            Check(Write(name, (uint)kind, bytes), "write " + name);
        }
        public void DeleteValue(string name, bool throwOnMissing = false) { Check(unchecked((uint)Clear(name)), "clear " + name); }
        public void Dispose() { if (handle != IntPtr.Zero) { if (batch == null) AmdgpuClientClose(handle); handle = IntPtr.Zero; } }
    }

    // The native writer mutex is owned by a thread. This synchronous scope must
    // cover raw backup, every setting write, verification and any rollback.
    public sealed class DriverStateBatch : IDisposable
    {
        IntPtr handle;
        readonly int thread = System.Threading.Thread.CurrentThread.ManagedThreadId;
        DriverStateBatch(IntPtr handle) { this.handle = handle; }
        internal IntPtr Handle
        {
            get
            {
                if (thread != System.Threading.Thread.CurrentThread.ManagedThreadId)
                    throw new InvalidOperationException("Driver state batch belongs to another thread");
                if (handle == IntPtr.Zero) throw new ObjectDisposedException("DriverStateBatch");
                return handle;
            }
        }
        public static DriverStateBatch Begin()
        {
            IntPtr handle;
            DriverStateKey.Check(DriverStateKey.AmdgpuClientBeginBatch(out handle), "begin driver state change");
            return new DriverStateBatch(handle);
        }
        public DriverStateKey Open(string child = "", bool effective = true)
        { return DriverStateKey.OpenBatch(this, child, effective); }
        public DriverStateKey OpenPath(string path, bool effective = true)
        {
            if (!DriverStateKey.IsPath(path)) throw new ArgumentException("Not a driver state identifier", "path");
            return Open(path.Length == DpmSettings.RegistryPath.Length ? "" : path.Substring(DpmSettings.RegistryPath.Length + 1), effective);
        }
        public void Dispose()
        {
            if (handle == IntPtr.Zero) return;
            DriverStateKey.Check(DriverStateKey.AmdgpuClientEndBatch(Handle), "end driver state change");
            handle = IntPtr.Zero;
        }
    }
}
