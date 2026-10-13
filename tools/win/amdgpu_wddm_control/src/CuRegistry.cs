// The CU setter retains one common state batch through its checked sequence and
// readback. CheckedCuRegistry is pure over IRegApi for injected native failures.
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

        // An absent value counts as removed; every other status is an error. CuMode itself is never removed: the
        // stock choice is written as 24, so that the release installer reads it as a choice and not as a key it
        // never configured (BD-077).
        public void Delete(string name)
        {
            if (name != "CuDisableWgp" && name != "CuModeConfirmed" && name != "CuModeClosedReason") throw new InvalidOperationException(name + " is not a value the setter removes");
            int s = _api.Delete(name);
            if (s != Success && s != FileNotFound) throw new CuRegistryException("delete", name, s);
        }

        public void SetDword(string name, uint value)
        {
            if (name != "CuMode" || (value != CuMode.Full && value != CuMode.Stock)) throw new InvalidOperationException("the setter writes only CuMode = 40 or CuMode = 24");
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
        readonly DriverStateKey state;
        readonly DriverStateBatch ownedBatch;
        public Win32RegApi(string path, DriverStateBatch batch = null)
        {
            if (batch == null) batch = ownedBatch = DriverStateBatch.Begin();
            try { state = batch.OpenPath(path); }
            catch { if (ownedBatch != null) ownedBatch.Dispose(); throw; }
        }
        public int Query(string name, out uint type, out byte[] data) { return state.Query(name, out type, out data); }
        public int Delete(string name) { return state.Clear(name); }
        public int SetDword(string name, uint value) { return state.SetDword(name, value); }
        // Every common-adapter mutation already includes a checked flush.
        public int Flush() { return 0; }
        public void Dispose() { try { state.Dispose(); } finally { if (ownedBatch != null) ownedBatch.Dispose(); } }
    }
}
