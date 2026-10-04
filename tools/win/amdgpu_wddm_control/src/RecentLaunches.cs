// Recent launches (A4, WU-007, WU-012, WU-075; docs/gui/interfaces.md section 3): written by the UMD frontends after
// a successful outer device creation, read, pruned and cleared here. Per user, local only. The app never invents a
// launch: an entry with a missing value, a wrong type or a key that is not the hash of its path is skipped. Labels
// say "launched", never "played".
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class RecentLaunch
    {
        public string Key, Path, Image, Api;
        public DateTime LastLaunchUtc;
        public uint Launches;
    }

    // One subkey's values as read (uint for REG_DWORD, ulong for REG_QWORD, string for REG_SZ); null: unreadable.
    public interface IRecentStore
    {
        IList<string> Keys();
        IDictionary<string, object> Values(string key);
        void DeleteKey(string key);
        // Takes the commit mutex within the bound; false when another writer holds it.
        bool Lock(int milliseconds);
        void Unlock();
    }

    public sealed class MemoryRecentStore : IRecentStore
    {
        public readonly Dictionary<string, Dictionary<string, object>> Data = new Dictionary<string, Dictionary<string, object>>(StringComparer.OrdinalIgnoreCase);
        public bool Busy;
        public int Deleted;
        public IList<string> Keys() { return Data.Keys.ToList(); }
        public IDictionary<string, object> Values(string key) { Dictionary<string, object> v; return Data.TryGetValue(key, out v) ? v : null; }
        public void DeleteKey(string key) { if (Data.Remove(key)) Deleted++; }
        public bool Lock(int milliseconds) { return !Busy; }
        public void Unlock() { }
    }

    public sealed class RegistryRecentStore : IRecentStore
    {
        Mutex _mutex;

        public IList<string> Keys()
        {
            try { using (var k = Registry.CurrentUser.OpenSubKey(RecentLaunches.Path)) return k == null ? new List<string>() : k.GetSubKeyNames().ToList(); }
            catch (Exception) { return new List<string>(); }
        }

        public IDictionary<string, object> Values(string key)
        {
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RecentLaunches.Path + "\\" + key))
                {
                    if (k == null) return null;
                    var d = new Dictionary<string, object>(StringComparer.OrdinalIgnoreCase);
                    foreach (var n in k.GetValueNames())
                    {
                        var v = k.GetValue(n);
                        var kind = k.GetValueKind(n);
                        d[n] = kind == RegistryValueKind.DWord ? (object)unchecked((uint)(int)v) : kind == RegistryValueKind.QWord ? (object)unchecked((ulong)(long)v)
                            : kind == RegistryValueKind.String ? v : null;
                    }
                    return d;
                }
            }
            catch (Exception) { return null; }
        }

        public void DeleteKey(string key)
        {
            try { using (var k = Registry.CurrentUser.OpenSubKey(RecentLaunches.Path, true)) if (k != null) k.DeleteSubKeyTree(key, false); }
            catch (Exception) { }
        }

        public bool Lock(int milliseconds)
        {
            try
            {
                _mutex = new Mutex(false, RecentLaunches.MutexName);
                try { if (_mutex.WaitOne(milliseconds)) return true; }
                catch (AbandonedMutexException) { return true; }
                _mutex.Dispose(); _mutex = null;
                return false;
            }
            catch (Exception) { return false; }
        }

        public void Unlock()
        {
            if (_mutex == null) return;
            try { _mutex.ReleaseMutex(); } catch (Exception) { }
            _mutex.Dispose(); _mutex = null;
        }
    }

    public enum ClearResult { Cleared, Busy }

    public static class RecentLaunches
    {
        public const string Path = @"Software\amdgpu-wddm\RecentLaunches";
        public const string SwitchName = "RecordRecentLaunches";
        public const string MutexName = @"Local\amdgpu-wddm-recent-launches";
        public const int Keep = 50, ClearWaitMs = 2000;
        static readonly string[] Apis = { "D3D12", "D3D11", "Vulkan" };

        // The writers' normalization: a \\?\ or \\?\UNC\ prefix removed (UNC becomes \\), then the simple invariant upper
        // case. The writer has already made the path full (GetFullPathNameW).
        public static string Normalize(string path)
        {
            var p = path ?? "";
            if (p.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) p = @"\\" + p.Substring(8);
            else if (p.StartsWith(@"\\?\", StringComparison.Ordinal)) p = p.Substring(4);
            return p.ToUpperInvariant();
        }

        // The subkey name: 32 lower-case hex digits, the first 16 bytes of SHA-256 over the UTF-16LE normalized path.
        public static string KeyOf(string path)
        {
            using (var sha = SHA256.Create())
            {
                var h = sha.ComputeHash(Encoding.Unicode.GetBytes(Normalize(path)));
                var w = new StringBuilder(32);
                for (int i = 0; i < 16; i++) w.Append(h[i].ToString("x2", CultureInfo.InvariantCulture));
                return w.ToString();
            }
        }

        // One subkey's record; null when any value is missing, of another type or does not agree with the key.
        public static RecentLaunch Parse(string key, IDictionary<string, object> v)
        {
            if (v == null || key == null || key.Length != 32 || key.Any(c => !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f'))) return null;
            object path, image, last, api, n;
            if (!v.TryGetValue("Path", out path) || !(path is string) || !v.TryGetValue("Image", out image) || !(image is string) ||
                !v.TryGetValue("LastLaunchUtc", out last) || !(last is ulong) || !v.TryGetValue("Api", out api) || !(api is string) ||
                !v.TryGetValue("Launches", out n) || !(n is uint)) return null;
            var p = (string)path;
            if (p.Length == 0 || KeyOf(p) != key) return null;
            string file;
            try { file = System.IO.Path.GetFileName(p); } catch (ArgumentException) { return null; }
            if (!string.Equals(file, (string)image, StringComparison.OrdinalIgnoreCase) || !Apis.Contains((string)api)) return null;
            DateTime when;
            try { when = DateTime.FromFileTimeUtc(unchecked((long)(ulong)last)); } catch (ArgumentOutOfRangeException) { return null; }
            return new RecentLaunch { Key = key, Path = p, Image = (string)image, Api = (string)api, LastLaunchUtc = when, Launches = (uint)n };
        }

        // Every valid entry, newest first. Reading takes no lock: a half-written entry fails Parse and is skipped.
        public static List<RecentLaunch> Read(IRecentStore store)
        {
            var list = new List<RecentLaunch>();
            foreach (var key in store.Keys())
            {
                var r = Parse(key, store.Values(key));
                if (r != null) list.Add(r);
            }
            return list.OrderByDescending(r => r.LastLaunchUtc).ThenBy(r => r.Path, StringComparer.OrdinalIgnoreCase).ToList();
        }

        // Prune under the mutex: the Keep newest valid entries stay; invalid subkeys are left to their writer. Returns the
        // number removed, -1 when the mutex was busy (nothing removed).
        public static int Prune(IRecentStore store)
        {
            if (!store.Lock(0)) return -1;
            try
            {
                int removed = 0;
                foreach (var old in Read(store).Skip(Keep)) { store.DeleteKey(old.Key); removed++; }
                return removed;
            }
            finally { store.Unlock(); }
        }

        // "Clear the list": under the mutex (2 s bound) every subkey goes. Busy: nothing was removed, say so.
        public static ClearResult Clear(IRecentStore store)
        {
            if (!store.Lock(ClearWaitMs)) return ClearResult.Busy;
            try { foreach (var key in store.Keys()) store.DeleteKey(key); return ClearResult.Cleared; }
            finally { store.Unlock(); }
        }
    }
}
