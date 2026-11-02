# Native contracts: Windows SDK 10.0.26100.0, fileapi.h BY_HANDLE_FILE_INFORMATION,
# minwinbase.h FILE_INFO_BY_HANDLE_CLASS, winbase.h FILE_DISPOSITION_INFO.
# All hashes and writes use the same open file. No file handle is released between validation and mutation.
if (-not ('Bc250.RocmFiles' -as [type])) {
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;
namespace Bc250 {
public sealed class RocmFiles : IDisposable {
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct FileInformation {
        public uint Attributes, CreatedLow, CreatedHigh, AccessedLow, AccessedHigh, WrittenLow, WrittenHigh;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateDirectoryW(string name, IntPtr security);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out FileInformation info);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle handle, int kind, ref int value, uint size);
    sealed class OpenFile {
        public FileStream Stream;
        public bool Writable, Created, Delete;
        public byte[] Before;
    }
    readonly Dictionary<string, OpenFile> files = new Dictionary<string, OpenFile>(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, SafeFileHandle> dirs = new Dictionary<string, SafeFileHandle>(StringComparer.OrdinalIgnoreCase);
    bool committed;
    public static string LocalPath(string path) {
        if (path == null || path.Length < 4 || !Char.IsLetter(path[0]) || path[1] != ':' || path[2] != '\\' ||
            path[3] == '\\' || path.Substring(2).Contains(":")) throw new IOException("not a folder on a local drive letter: " + path);
        foreach (string part in path.Substring(3).Split('\\'))
            if (part != "." && part != ".." && (part.EndsWith(".") || part.EndsWith(" "))) throw new IOException("ambiguous path: " + path);
        string full = Path.GetFullPath(path).TrimEnd('\\');
        if (full.Length < 4) throw new IOException("drive root is not a venv or list: " + path);
        return full;
    }
    public RocmFiles() {
        if (Marshal.SizeOf(typeof(FileInformation)) != 52 ||
            Marshal.OffsetOf(typeof(FileInformation), "Volume").ToInt32() != 28 ||
            Marshal.OffsetOf(typeof(FileInformation), "Links").ToInt32() != 40 ||
            Marshal.OffsetOf(typeof(FileInformation), "IndexHigh").ToInt32() != 44)
            throw new InvalidOperationException("BY_HANDLE_FILE_INFORMATION layout mismatch");
    }
    static Exception Error(string path) {
        int code = Marshal.GetLastWin32Error();
        if (code == 32 || code == 33) return new IOException("IN USE: " + path + " (close every Python process)");
        return new IOException(path + ": " + new Win32Exception(code).Message);
    }
    static FileInformation Info(SafeFileHandle handle, string path, bool directory) {
        FileInformation info;
        if (!GetFileInformationByHandle(handle, out info)) throw Error(path);
        if ((info.Attributes & 0x400) != 0) throw new IOException(path + " is a junction or a link");
        if (((info.Attributes & 0x10) != 0) != directory) throw new IOException(path + " has the wrong file type");
        if (!directory && info.Links != 1) throw new IOException(path + " is a hard link");
        return info;
    }
    // Parents are held before a child is opened or created. Deny write and delete sharing:
    // neither rename nor FSCTL_SET_REPARSE_POINT can redirect a retained directory.
    public bool HoldDirectory(string directory, bool create, bool optional) {
        string full = LocalPath(directory);
        string root = Path.GetPathRoot(full), current = root;
        List<string> chain = new List<string>(); chain.Add(root);
        foreach (string part in full.Substring(root.Length).Split(new char[] {'\\'}, StringSplitOptions.RemoveEmptyEntries)) {
            current = Path.Combine(current, part); chain.Add(current);
        }
        foreach (string path in chain) {
            if (dirs.ContainsKey(path)) continue;
            SafeFileHandle handle = CreateFileW(path, 0x80, 1, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
            if (handle.IsInvalid) {
                int code = Marshal.GetLastWin32Error(); handle.Dispose();
                if (code != 2 && code != 3) throw Error(path);
                if (!create) { if (optional) return false; throw new DirectoryNotFoundException("MISSING: " + path); }
                if (!CreateDirectoryW(path, IntPtr.Zero) && Marshal.GetLastWin32Error() != 183) throw Error(path);
                handle = CreateFileW(path, 0x80, 1, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
                if (handle.IsInvalid) { handle.Dispose(); throw Error(path); }
            }
            try { Info(handle, path, true); dirs.Add(path, handle); }
            catch { handle.Dispose(); throw; }
        }
        return true;
    }
    public bool Open(string path, bool writable, bool create) {
        OpenFile old;
        if (files.TryGetValue(path, out old)) {
            if (writable && !old.Writable) throw new IOException("read-only input aliases an output: " + path);
            return true;
        }
        if (!dirs.ContainsKey(Path.GetDirectoryName(path))) throw new IOException("parent was not retained: " + path);
        SafeFileHandle handle = CreateFileW(path, writable ? 0xC0010000u : 0x80000000u,
            writable ? 0u : 1u, IntPtr.Zero, create ? 1u : 3u, 0x00200000, IntPtr.Zero);
        if (handle.IsInvalid) {
            int code = Marshal.GetLastWin32Error(); handle.Dispose();
            if (!create && code == 2) return false;
            throw Error(path);
        }
        try {
            Info(handle, path, false);
            FileStream stream = new FileStream(handle, writable ? FileAccess.ReadWrite : FileAccess.Read);
            files.Add(path, new OpenFile { Stream = stream, Writable = writable, Created = create });
            return true;
        } catch { handle.Dispose(); throw; }
    }
    public bool Exists(string path) { return files.ContainsKey(path); }
    public byte[] Read(string path, int limit) {
        OpenFile f;
        if (!files.TryGetValue(path, out f)) return new byte[0];
        if (f.Stream.Length > limit) throw new IOException("file exceeds limit: " + path);
        f.Stream.Position = 0;
        byte[] data = new byte[(int)f.Stream.Length];
        int n = 0; while (n < data.Length) { int got = f.Stream.Read(data, n, data.Length - n); if (got == 0) throw new EndOfStreamException(path); n += got; }
        return data;
    }
    public string Text(string path) { return Encoding.UTF8.GetString(Read(path, 1024 * 1024)).TrimStart('\uFEFF'); }
    public string Hash(string path) {
        OpenFile f; if (!files.TryGetValue(path, out f)) return "";
        f.Stream.Position = 0;
        using (SHA256 sha = SHA256.Create()) { return BitConverter.ToString(sha.ComputeHash(f.Stream)).Replace("-", "").ToLowerInvariant(); }
    }
    void Snapshot(string path) {
        OpenFile f = files[path];
        if (!f.Writable) throw new IOException("read-only file: " + path);
        // Windows can add a hardlink even while this identity is held with no sharing.
        // Recheck links before each mutation. A later alias can only name this retained
        // identity; it cannot substitute a foreign file for the validated handle.
        Info(f.Stream.SafeFileHandle, path, false);
        if (!f.Created && f.Before == null) f.Before = Read(path, 512 * 1024 * 1024);
    }
    static void WriteStream(FileStream stream, byte[] data) {
        stream.Position = 0; stream.Write(data, 0, data.Length); stream.SetLength(data.Length); stream.Flush(true);
    }
    public void Write(string path, byte[] data) { Snapshot(path); WriteStream(files[path].Stream, data); }
    public void WriteText(string path, string text) { Write(path, new UTF8Encoding(false).GetBytes(text)); }
    public void Copy(string source, string destination) { Write(destination, Read(source, 512 * 1024 * 1024)); }
    public void Delete(string path) { if (files.ContainsKey(path)) { Snapshot(path); files[path].Delete = true; } }
    static void Disposition(OpenFile f, bool delete) {
        int value = delete ? 1 : 0;
        if (!SetFileInformationByHandle(f.Stream.SafeFileHandle, 4, ref value, 4)) throw Error("file disposition");
    }
    public void Commit() {
        foreach (OpenFile f in files.Values) if (f.Delete) Disposition(f, true);
        committed = true;
    }
    public void Dispose() {
        Exception rollback = null;
        if (!committed) foreach (OpenFile f in files.Values) {
            try {
                if (f.Created) Disposition(f, true);
                else if (f.Before != null) { Disposition(f, false); WriteStream(f.Stream, f.Before); }
            } catch (Exception e) { rollback = e; }
        }
        foreach (OpenFile f in files.Values) f.Stream.Dispose();
        foreach (SafeFileHandle h in dirs.Values) h.Dispose();
        if (rollback != null) throw new IOException("ROLLBACK FAILED: " + rollback.Message, rollback);
    }
}
}
'@
}
