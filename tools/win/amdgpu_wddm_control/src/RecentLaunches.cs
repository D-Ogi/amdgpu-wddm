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

    // What a recorded launch is, as far as its path tells (BD-140). The list records every program that created a
    // Direct3D device on this driver (docs/design/recent-launches.md: "launched", not "a game"), so a web browser's
    // GPU process, a web view inside another program and a Windows tool such as the Snipping Tool are in it too. The
    // window offers only Game as a game; the others stay in the list, hidden unless the person asks for them.
    public enum LaunchKind { Game, Browser, Windows, Launcher, OwnTool }

    // The rule is a list of what is known not to be a game, never a list of where games live: a game in any folder
    // of any drive stays a game. Only the recorded path is read, nothing is opened (the window builds its pages from
    // this), so a name the lists below do not know is a game.
    public static class LaunchKinds
    {
        // Web browsers, and the programs that draw web pages inside other programs: Edge's WebView2 (Widgets, Teams,
        // the new Outlook, the Xbox app), the Chromium Embedded Framework helpers of game stores and other programs,
        // Qt WebEngine. Their GPU processes create Direct3D 11 and 12 devices like a game does.
        static readonly HashSet<string> BrowserFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "msedge.exe", "msedgewebview2.exe", "iexplore.exe", "chrome.exe", "chromium.exe", "firefox.exe", "brave.exe",
            "opera.exe", "vivaldi.exe", "librewolf.exe", "waterfox.exe", "floorp.exe", "thorium.exe",
            "steamwebhelper.exe", "EpicWebHelper.exe", "UplayWebCore.exe", "CefSharp.BrowserSubprocess.exe", "QtWebEngineProcess.exe",
        };

        // The game stores' own clients: they start games, they are not games.
        static readonly HashSet<string> LauncherFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "steam.exe", "EpicGamesLauncher.exe", "EADesktop.exe", "Origin.exe", "UbisoftConnect.exe", "upc.exe", "GalaxyClient.exe",
            "Battle.net.exe", "XboxPcApp.exe",
        };

        // Packaged programs that come with Windows or from Microsoft and are not games, by the package name (the part
        // of the package folder before the first "_"; WindowsApps\Microsoft.ScreenSketch_11.2409.25.0_x64__8wekyb3d8bbwe
        // is the Snipping Tool). A name, not the publisher: Microsoft's own games come as packages of the same
        // publisher (Microsoft Solitaire Collection, Forza), and a package this list does not name stays a game.
        static readonly HashSet<string> WindowsPackages = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "Microsoft.ScreenSketch", "Microsoft.Windows.Photos", "Microsoft.Paint", "Microsoft.MSPaint", "Microsoft.WindowsNotepad",
            "Microsoft.WindowsTerminal", "Microsoft.WindowsTerminalPreview", "Microsoft.WindowsCalculator", "Microsoft.WindowsCamera",
            "Microsoft.WindowsAlarms", "Microsoft.WindowsSoundRecorder", "Microsoft.WindowsMaps", "Microsoft.ZuneMusic", "Microsoft.ZuneVideo",
            "Microsoft.MicrosoftStickyNotes", "Microsoft.Todos", "Microsoft.OutlookForWindows", "microsoft.windowscommunicationsapps",
            "Microsoft.People", "Microsoft.GamingApp", "Microsoft.XboxApp", "Microsoft.XboxGamingOverlay", "Microsoft.XboxGameOverlay",
            "Microsoft.XboxIdentityProvider", "Microsoft.XboxSpeechToTextOverlay", "Microsoft.Xbox.TCUI", "Microsoft.YourPhone",
            "MicrosoftWindows.CrossDevice", "MicrosoftWindows.Client.WebExperience", "Microsoft.WidgetsPlatformRuntime",
            "Microsoft.Windows.DevHome", "Microsoft.Copilot", "Microsoft.Windows.Ai.Copilot.Provider", "Microsoft.MicrosoftOfficeHub",
            "Microsoft.Office.OneNote", "MSTeams", "MicrosoftTeams", "Microsoft.WindowsStore", "Microsoft.StorePurchaseApp",
            "Microsoft.DesktopAppInstaller", "Microsoft.BingNews", "Microsoft.BingWeather", "Microsoft.BingSearch", "Microsoft.GetHelp",
            "Microsoft.Getstarted", "Microsoft.WindowsFeedbackHub", "Microsoft.PowerAutomateDesktop", "Clipchamp.Clipchamp",
            "MicrosoftCorporationII.QuickAssist", "Microsoft.MicrosoftEdge", "Microsoft.MicrosoftEdge.Stable", "Microsoft.MicrosoftEdge.Beta",
            "Microsoft.MicrosoftEdge.Dev", "Microsoft.MicrosoftEdge.Canary", "Microsoft.Win32WebViewHost", "Microsoft.Whiteboard",
            "Microsoft.Microsoft3DViewer", "Microsoft.MixedReality.Portal", "Microsoft.SkypeApp", "Microsoft.549981C3F5F10",
        };

        // windowsDir: the Windows directory of this PC (the writers skip it already; an older list may still hold
        // such a path). Compared as a directory: C:\Windows2\x.exe is not below C:\Windows.
        public static LaunchKind Of(string path, string windowsDir)
        {
            if (string.IsNullOrEmpty(path)) return LaunchKind.Game;
            string file;
            try { file = System.IO.Path.GetFileName(path); } catch (ArgumentException) { return LaunchKind.Game; }
            if (Below(path, windowsDir)) return LaunchKind.Windows;
            // This project's own programs, as the writers name them (amdgpu_wddm_*, bc250*, vulkaninfo*).
            if (file.StartsWith("amdgpu_wddm_", StringComparison.OrdinalIgnoreCase) || file.StartsWith("bc250", StringComparison.OrdinalIgnoreCase) ||
                file.StartsWith("vulkaninfo", StringComparison.OrdinalIgnoreCase)) return LaunchKind.OwnTool;
            if (BrowserFiles.Contains(file)) return LaunchKind.Browser;
            if (LauncherFiles.Contains(file)) return LaunchKind.Launcher;
            var package = PackageName(path);
            if (package != null && WindowsPackages.Contains(package)) return LaunchKind.Windows;
            return LaunchKind.Game;
        }

        public static LaunchKind Of(RecentLaunch launch, string windowsDir) { return launch == null ? LaunchKind.Game : Of(launch.Path, windowsDir); }

        // The package name of a path inside a WindowsApps folder (C:\Program Files\WindowsApps, or X:\WindowsApps
        // of a drive the person moved apps to): the folder after it is "<name>_<version>_<arch>_<resource>_<publisher id>".
        public static string PackageName(string path)
        {
            var parts = (path ?? "").Split('\\', '/');
            for (int i = 0; i + 2 < parts.Length; i++)
                if (parts[i].Equals("WindowsApps", StringComparison.OrdinalIgnoreCase))
                {
                    var folder = parts[i + 1];
                    int cut = folder.IndexOf('_');
                    return cut > 0 && folder.Split('_').Length >= 5 ? folder.Substring(0, cut) : null;
                }
            return null;
        }

        static bool Below(string path, string dir)
        {
            if (string.IsNullOrEmpty(dir)) return false;
            var d = dir.TrimEnd('\\', '/');
            return d.Length > 0 && path.Length > d.Length + 1 && path.StartsWith(d, StringComparison.OrdinalIgnoreCase) && (path[d.Length] == '\\' || path[d.Length] == '/');
        }

        // Whether an entry of the Games list is hidden: the person hid it, or it is not a game and the person has not
        // asked to see it. "Show hidden games" lists both; "Show" on an entry that is not a game puts it among the
        // games (shown), "Hide" takes it out again.
        public static bool Hidden(string image, LaunchKind kind, ISet<string> hidden, ISet<string> shown)
        {
            if (hidden != null && hidden.Contains(image)) return true;
            return kind != LaunchKind.Game && (shown == null || !shown.Contains(image));
        }

        // The person's choice on one entry: the new hidden and shown sets.
        public static void Toggle(string image, LaunchKind kind, bool show, ISet<string> hidden, ISet<string> shown)
        {
            if (show)
            {
                hidden.Remove(image);
                if (kind != LaunchKind.Game) shown.Add(image);
            }
            else
            {
                shown.Remove(image);
                if (kind == LaunchKind.Game) hidden.Add(image);
            }
        }

        // The entry's line under its name in the Games list: what it is, for an entry that is not a game.
        public static string TextId(LaunchKind kind)
        {
            switch (kind)
            {
                case LaunchKind.Browser: return "games.kind.browser";
                case LaunchKind.Windows: return "games.kind.windows";
                case LaunchKind.Launcher: return "games.kind.launcher";
                case LaunchKind.OwnTool: return "games.kind.own";
                default: return null;
            }
        }
    }
}
