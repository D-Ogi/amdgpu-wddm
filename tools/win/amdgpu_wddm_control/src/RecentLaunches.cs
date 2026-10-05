// Recent launches (A4, WU-007, WU-012, WU-075): the list that the D3D12 and D3D11 shells write, read and cleared here.
// The contract is docs/design/recent-launches.md (format version 1, stream C; docs/gui/interfaces.md section 3). Per
// user, local only. The app never writes the list and never invents a launch: a list that is not valid as a whole
// shows no entries, never a part. Labels say "launched", never "played".
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;

namespace AmdgpuWddmControl
{
    public sealed class RecentLaunch
    {
        public string Path, Image;
        public DateTime LastLaunchUtc;      // creation time of the process of the last launch
        public uint Pid, Launches, Apis;

        // The API of the last launch as the window names it: bit 1 is D3D12, everything else D3D11.
        public bool D3D12 { get { return (Apis & RecentLaunches.ApiD3D12) != 0; } }
    }

    public enum RecentListState { Missing, Valid, Invalid, OtherVersion }

    public sealed class RecentList
    {
        public RecentListState State;
        public List<RecentLaunch> Entries = new List<RecentLaunch>();
    }

    // The switch as the shells read it: absent or DWORD 1 = on, DWORD 0 = off, anything else records nothing.
    public enum RecentSwitch { On, Off, Invalid }

    // The list's files, so that tests run against memory and the window against %LOCALAPPDATA%.
    public interface IRecentFiles
    {
        // The list's bytes, at most max + 1 of them; null when the file does not exist. Throws when it cannot be read.
        byte[] ReadList(int max);
        // One attempt at the lock on byte 0 of the lock file, no wait.
        bool TryLock();
        void Unlock();
        // Deletes the list and the writer's temporary file; never the lock file.
        void DeleteList();
    }

    public sealed class MemoryRecentFiles : IRecentFiles
    {
        public byte[] List;
        public bool Unreadable;
        public int BusyAttempts;            // TryLock fails this many times first (int.MaxValue: always)
        public int Attempts, Deletes;
        public bool Locked;
        public byte[] ReadList(int max) { if (Unreadable) throw new IOException("unreadable"); return List; }
        public bool TryLock() { Attempts++; if (BusyAttempts > 0) { BusyAttempts--; return false; } Locked = true; return true; }
        public void Unlock() { Locked = false; }
        public void DeleteList() { if (!Locked) throw new InvalidOperationException("delete without the lock"); List = null; Deletes++; }
    }

    public sealed class LocalRecentFiles : IRecentFiles
    {
        readonly string _dir;
        FileStream _lock;

        public LocalRecentFiles() : this(System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), RecentLaunches.StoreDir)) { }
        public LocalRecentFiles(string dir) { _dir = dir; }

        string In(string name) { return System.IO.Path.Combine(_dir, name); }

        public byte[] ReadList(int max)
        {
            FileStream fs;
            try { fs = new FileStream(In(RecentLaunches.ListName), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); }
            catch (FileNotFoundException) { return null; }
            catch (DirectoryNotFoundException) { return null; }
            using (fs)
            {
                var buf = new byte[max + 1];
                int n = 0, r;
                while (n < buf.Length && (r = fs.Read(buf, n, buf.Length - n)) > 0) n += r;
                var data = new byte[n];
                Array.Copy(buf, data, n);
                return data;
            }
        }

        public bool TryLock()
        {
            try
            {
                if (_lock == null)
                {
                    Directory.CreateDirectory(_dir);
                    _lock = new FileStream(In(RecentLaunches.LockName), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete);
                }
                _lock.Lock(0, 1);
                return true;
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }

        public void Unlock()
        {
            if (_lock == null) return;
            try { _lock.Unlock(0, 1); } catch (IOException) { }
            _lock.Dispose(); _lock = null;
        }

        public void DeleteList()
        {
            File.Delete(In(RecentLaunches.ListName));
            File.Delete(In(RecentLaunches.TempName));
        }
    }

    public enum ClearResult { Cleared, Busy, Failed }

    public static class RecentLaunches
    {
        public const string SwitchName = "RecordRecentLaunches";
        public const string StoreDir = "amdgpu-wddm", ListName = "recent-launches.txt", LockName = "recent-launches.lock", TempName = "recent-launches.tmp";
        public const string HeaderPrefix = "amdgpu-wddm recent-launches ", Header = HeaderPrefix + "1\n";
        public const int MaxEntries = 64, MaxBytes = 8 << 20, ClearWaitMs = 2000, ClearRetryMs = 20;
        public const uint ApiD3D12 = 1;

        static readonly Encoding Utf8Strict = new UTF8Encoding(false, true);

        // Strict parse of a whole list (docs/design/recent-launches.md "Format"). Anything that is not one valid version 1
        // list is Invalid or, for another version's header, OtherVersion; neither has entries.
        public static RecentList Parse(byte[] data)
        {
            if (data == null) return new RecentList { State = RecentListState.Missing };
            var bad = new RecentList { State = RecentListState.Invalid };
            if (data.Length > MaxBytes) return bad;
            string text;
            try { text = Utf8Strict.GetString(data); } catch (DecoderFallbackException) { return bad; }
            if (!text.StartsWith(Header, StringComparison.Ordinal))
            {
                int eol = text.IndexOf('\n');
                ulong version;
                if (eol > HeaderPrefix.Length && text.StartsWith(HeaderPrefix, StringComparison.Ordinal) && Number(text.Substring(HeaderPrefix.Length, eol - HeaderPrefix.Length), false, out version) && version != 1)
                    return new RecentList { State = RecentListState.OtherVersion };
                return bad;
            }
            var list = new List<RecentLaunch>();
            int pos = Header.Length;
            while (true)
            {
                int eol = text.IndexOf('\n', pos);
                if (eol < 0) return bad;
                var line = text.Substring(pos, eol - pos);
                pos = eol + 1;
                if (line.StartsWith("end ", StringComparison.Ordinal))
                {
                    ulong count;
                    if (!Number(line.Substring(4), false, out count) || count != (ulong)list.Count || pos != text.Length) return bad;
                    return new RecentList { State = RecentListState.Valid, Entries = list };
                }
                var f = line.Split(new[] { '\t' }, 5);
                ulong start, pid, starts, apis;
                if (f.Length != 5 || !Number(f[0], true, out start) || !Number(f[1], false, out pid) || pid > uint.MaxValue ||
                    !Number(f[2], false, out starts) || starts > uint.MaxValue || !Number(f[3], false, out apis) || apis > uint.MaxValue) return bad;
                var path = f[4];
                if (path.Length == 0) return bad;
                foreach (var c in path) if (c < 0x20) return bad;
                string image;
                try { image = System.IO.Path.GetFileName(path); } catch (ArgumentException) { return bad; }
                DateTime when;
                try { when = DateTime.FromFileTimeUtc(unchecked((long)start)); } catch (ArgumentOutOfRangeException) { return bad; }
                list.Add(new RecentLaunch { Path = path, Image = image, LastLaunchUtc = when, Pid = (uint)pid, Launches = (uint)starts, Apis = (uint)apis });
                if (list.Count > MaxEntries) return bad;
            }
        }

        // Digits only, no sign or space; the start field is exactly 16 upper-case hexadecimal digits.
        static bool Number(string s, bool hex16, out ulong value)
        {
            value = 0;
            if (s.Length == 0 || (hex16 && s.Length != 16)) return false;
            foreach (var c in s)
                if (!(c >= '0' && c <= '9' || hex16 && c >= 'A' && c <= 'F')) return false;
            return ulong.TryParse(s, hex16 ? NumberStyles.AllowHexSpecifier : NumberStyles.None, CultureInfo.InvariantCulture, out value);
        }

        // Reads without the lock and never writes; a list that cannot be read is Invalid.
        public static RecentList Read(IRecentFiles files)
        {
            try { return Parse(files.ReadList(MaxBytes)); }
            catch (Exception) { return new RecentList { State = RecentListState.Invalid }; }
        }

        // "Clear the list" (docs/design/recent-launches.md "Clear and Off"): the lock, retried every 20 ms for up to
        // 2 s, then the list and the temporary file go. Run it off the UI thread. Busy: nothing was removed.
        public static ClearResult Clear(IRecentFiles files, Action<int> sleep = null)
        {
            sleep = sleep ?? Thread.Sleep;
            for (int attempt = 0; !files.TryLock(); attempt++)
            {
                if (attempt >= ClearWaitMs / ClearRetryMs) return ClearResult.Busy;
                sleep(ClearRetryMs);
            }
            try { files.DeleteList(); return ClearResult.Cleared; }
            catch (Exception) { return ClearResult.Failed; }
            finally { files.Unlock(); }
        }

        public static bool SamePath(string a, string b) { return string.Equals(a, b, StringComparison.OrdinalIgnoreCase); }
    }
}
