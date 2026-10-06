// The CU setter's registry access (review 927 R1): one handle to the driver's Parameters key, checked Win32 calls
// (RegQueryValueExW, RegDeleteValueW, RegSetValueExW, RegFlushKey) whose LSTATUS is kept, so that "not found" is told
// apart from every other failure and a failed flush is never hidden. CheckedCuRegistry is pure over IRegApi, so the
// tests inject native return codes; Win32RegApi is the live implementation the elevated helper uses.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace AmdgpuWddmControl
{
    // The four native calls, each returning its LSTATUS.
    public interface IRegApi
    {
        int Query(string name, out uint type, out byte[] data);
        int Delete(string name);
        int SetDword(string name, uint value);
        int Flush();
    }

    public sealed class CuRegistryException : Exception
    {
        public readonly string Operation, Name;
        public readonly int Status;

        public CuRegistryException(string operation, string name, int status, string detail = null)
            : base(operation + (name != null ? " " + name : "") + ": " + (detail ?? "LSTATUS " + status + " (" + new Win32Exception(status).Message + ")"))
        {
            Operation = operation; Name = name; Status = status;
        }
    }

    public sealed class CheckedCuRegistry : ICuRegistry
    {
        public const int Success = 0, FileNotFound = 2;
        const uint RegDword = 4;
        readonly IRegApi _api;

        public CheckedCuRegistry(IRegApi api) { _api = api; }

        // Absent (ERROR_FILE_NOT_FOUND) -> null; any other status, another type or another size -> an exception.
        public uint? Read(string name)
        {
            uint type; byte[] data;
            int s = _api.Query(name, out type, out data);
            if (s == FileNotFound) return null;
            if (s != Success) throw new CuRegistryException("read", name, s);
            if (type != RegDword || data == null || data.Length != 4) throw new CuRegistryException("read", name, s, "type " + type + ", " + (data == null ? 0 : data.Length) + " bytes, not a REG_DWORD");
            return BitConverter.ToUInt32(data, 0);
        }

        // An absent value counts as removed; every other status is an error.
        public void Delete(string name)
        {
            if (name != "CuMode" && name != "CuDisableWgp" && name != "CuModeConfirmed") throw new InvalidOperationException(name + " is not a value the setter removes");
            int s = _api.Delete(name);
            if (s != Success && s != FileNotFound) throw new CuRegistryException("delete", name, s);
        }

        public void SetDword(string name, uint value)
        {
            if (name != "CuMode" || value != CuMode.Full) throw new InvalidOperationException("the setter writes only CuMode = 40");
            int s = _api.SetDword(name, value);
            if (s != Success) throw new CuRegistryException("set", name, s);
        }

        public void Flush()
        {
            int s = _api.Flush();
            if (s != Success) throw new CuRegistryException("flush", null, s);
        }
    }

    public sealed class Win32RegApi : IRegApi, IDisposable
    {
        const int KeyQueryValue = 0x0001, KeySetValue = 0x0002, KeyWow64_64 = 0x0100;
        static readonly UIntPtr HkeyLocalMachine = new UIntPtr(0x80000002u);
        readonly SafeRegistryHandle _key;

        // Opens HKLM\<path> for query and set; throws with the LSTATUS when it cannot.
        public Win32RegApi(string path)
        {
            IntPtr h;
            int s = RegOpenKeyExW(HkeyLocalMachine, path, 0, KeyQueryValue | KeySetValue | KeyWow64_64, out h);
            if (s != 0) throw new CuRegistryException("open", @"HKLM\" + path, s);
            _key = new SafeRegistryHandle(h, true);
        }

        public int Query(string name, out uint type, out byte[] data)
        {
            data = null;
            int size = 0;
            int s = RegQueryValueExW(_key, name, IntPtr.Zero, out type, null, ref size);
            if (s != 0) return s;
            var buf = new byte[size];
            s = RegQueryValueExW(_key, name, IntPtr.Zero, out type, buf, ref size);
            if (s == 0) { data = new byte[size]; Array.Copy(buf, data, size); }
            return s;
        }

        public int Delete(string name) { return RegDeleteValueW(_key, name); }

        public int SetDword(string name, uint value) { return RegSetValueExW(_key, name, 0, 4, BitConverter.GetBytes(value), 4); }

        public int Flush() { return RegFlushKey(_key); }

        public void Dispose() { _key.Dispose(); }

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegOpenKeyExW(UIntPtr hKey, string subKey, int options, int samDesired, out IntPtr result);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegQueryValueExW(SafeRegistryHandle hKey, string valueName, IntPtr reserved, out uint type, byte[] data, ref int size);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegDeleteValueW(SafeRegistryHandle hKey, string valueName);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        static extern int RegSetValueExW(SafeRegistryHandle hKey, string valueName, int reserved, uint type, byte[] data, int size);

        [DllImport("advapi32.dll")]
        static extern int RegFlushKey(SafeRegistryHandle hKey);
    }
}
