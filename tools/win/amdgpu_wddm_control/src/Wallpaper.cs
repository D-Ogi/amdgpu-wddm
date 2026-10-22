// The Nagi wallpaper (after a successful install the wallpaper is set, unless the person cleared
// the box of the installer). The installer only records the choice: HKLM\SOFTWARE\amdgpu-wddm\Release Wallpaper
// (DWORD 1 or 0) and WallpaperStamp (REG_SZ, one value per install). This application sets the picture, in the
// session of the person who signed in and with that person's own rights, once per install stamp and only after the
// installer's state says that install was verified. Two existing starts reach it: the sign-in notice that the logon
// task "amdgpu-wddm start window" starts for every person (Program.StartNoticeRun, after the driver answered), and
// the first read of the window. Neither of them waits for it, and a failure is one line in the person's own log.
//
// Before it changes anything it saves the background the person had (HKCU\Software\amdgpu-wddm\Control
// WallpaperPrevious), and Settings, "Nagi wallpaper", sets the picture again or restores that background. The
// uninstaller leaves the person's current background alone. The picture ships as an obfuscated container
// (art/wallpaper/PROVENANCE.md), never as a plain image file.
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    // The .nwp container (art/wallpaper/PROVENANCE.md). Pure: bytes in, bytes out.
    public static class NagiWallpaperFile
    {
        public const string Name = "nagi-wallpaper.nwp";
        public const int HeaderBytes = 44;
        public const string KeyText = "Nagi's AMD Graphics Infrastructure";
        static readonly byte[] Magic = Encoding.ASCII.GetBytes("NAGIWP01");

        public static byte[] Key()
        {
            using (var h = SHA256.Create()) return h.ComputeHash(Encoding.ASCII.GetBytes(KeyText));
        }

        static byte[] Sha256(byte[] data)
        {
            using (var h = SHA256.Create()) return h.ComputeHash(data);
        }

        // The plain JPEG, or null with the reason. A file is refused when its magic, its length (the header's length
        // and the bytes that follow it must agree exactly) or the SHA-256 of the decoded picture is not right.
        public static byte[] Decode(byte[] data, out string problem)
        {
            problem = null;
            if (data == null || data.Length < HeaderBytes) { problem = "the file is shorter than its header"; return null; }
            for (int i = 0; i < Magic.Length; i++)
                if (data[i] != Magic[i]) { problem = "not a Nagi wallpaper file"; return null; }
            long n = (long)data[40] | (long)data[41] << 8 | (long)data[42] << 16 | (long)data[43] << 24;
            if (data.Length - HeaderBytes != n) { problem = "the header names " + n + " bytes and the file holds " + (data.Length - HeaderBytes); return null; }
            var key = Key();
            var plain = new byte[n];
            for (long i = 0; i < n; i++) plain[i] = (byte)(data[HeaderBytes + i] ^ key[i % 32]);
            var sum = Sha256(plain);
            for (int i = 0; i < 32; i++)
                if (sum[i] != data[8 + i]) { problem = "the SHA-256 of the picture is not the one its header names"; return null; }
            return plain;
        }

        // The other direction, for the tests (the release file is made by scratch tools, not by this application).
        public static byte[] Encode(byte[] plain)
        {
            var key = Key();
            var o = new byte[HeaderBytes + plain.Length];
            Array.Copy(Magic, o, Magic.Length);
            Array.Copy(Sha256(plain), 0, o, 8, 32);
            uint n = (uint)plain.Length;
            o[40] = (byte)n; o[41] = (byte)(n >> 8); o[42] = (byte)(n >> 16); o[43] = (byte)(n >> 24);
            for (int i = 0; i < plain.Length; i++) o[HeaderBytes + i] = (byte)(plain[i] ^ key[i % 32]);
            return o;
        }
    }

    public enum BackgroundKind { Unknown, Picture, Solid, Slideshow, Spotlight }

    // The background a person had before the Nagi wallpaper, as WallpaperPrevious stores it: one REG_SZ of five
    // fields joined by "|" (kind, picture path, WallpaperStyle, TileWallpaper, colour "r g b"). A path cannot hold
    // "|", and a REG_MULTI_SZ cannot hold the empty fields.
    public sealed class SavedBackground
    {
        public BackgroundKind Kind;
        public string Path = "", Style = "", Tile = "", Color = "";

        public string ToValue() { return string.Join("|", Kind.ToString().ToLowerInvariant(), Path ?? "", Style ?? "", Tile ?? "", Color ?? ""); }

        public static SavedBackground FromValue(string value)
        {
            var v = value == null ? null : value.Split('|');
            if (v == null || v.Length != 5) return null;
            BackgroundKind kind;
            if (!Enum.TryParse(v[0], true, out kind) || v[0].ToLowerInvariant() != kind.ToString().ToLowerInvariant()) return null;
            return new SavedBackground { Kind = kind, Path = v[1] ?? "", Style = v[2] ?? "", Tile = v[3] ?? "", Color = v[4] ?? "" };
        }
    }

    public enum WallpaperDue { Due, NotChosen, AlreadySet, NotVerified }

    public static class NagiWallpaper
    {
        // The sign-in run waits this long for the verification of the install it finished: the logon task starts it
        // at the same moment as the verification that the installer runs at that logon.
        public const int SignInWaitSeconds = 300, SignInPollMs = 5000;
        public const string ReleaseKey = @"SOFTWARE\amdgpu-wddm\Release", ChoiceValue = "Wallpaper", StampValue = "WallpaperStamp";
        // IDesktopWallpaper's DESKTOP_WALLPAPER_POSITION.
        public const int PositionCenter = 0, PositionTile = 1, PositionStretch = 2, PositionFit = 3, PositionFill = 4, PositionSpan = 5;

        // ---- the rules (pure) ----------------------------------------------------------------------------------

        // Due: the installer recorded the choice 1 with a stamp, this account has not had it set for that stamp, and
        // the installer's state says the installed release was verified.
        public static WallpaperDue Decide(object choice, string stamp, string appliedStamp, string phase, string statePackage, string installed)
        {
            bool chosen = (choice is int && (int)choice == 1) || (choice is uint && (uint)choice == 1);
            if (!chosen || string.IsNullOrEmpty(stamp)) return WallpaperDue.NotChosen;
            if (string.Equals(stamp, appliedStamp, StringComparison.Ordinal)) return WallpaperDue.AlreadySet;
            if (phase != "verified" || string.IsNullOrEmpty(installed) || !string.Equals(statePackage, installed, StringComparison.OrdinalIgnoreCase))
                return WallpaperDue.NotVerified;
            return WallpaperDue.Due;
        }

        // HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers BackgroundType: 0 picture, 1 solid
        // colour, 2 slideshow, 3 Windows Spotlight. Without it the wallpaper path decides between a picture and a
        // solid colour.
        public static BackgroundKind KindOf(object backgroundType, string wallpaperPath)
        {
            if (backgroundType is int)
                switch ((int)backgroundType)
                {
                    case 0: return BackgroundKind.Picture;
                    case 1: return BackgroundKind.Solid;
                    case 2: return BackgroundKind.Slideshow;
                    case 3: return BackgroundKind.Spotlight;
                    default: return BackgroundKind.Unknown;
                }
            return string.IsNullOrEmpty(wallpaperPath) ? BackgroundKind.Solid : BackgroundKind.Picture;
        }

        // A slideshow and Windows Spotlight show a picture that Windows keeps in a cache it rewrites: the picture is
        // copied before it is replaced, and a restore can bring back that one picture, not the slideshow itself.
        public static bool KeepsCopy(BackgroundKind kind, string path)
        {
            return kind == BackgroundKind.Slideshow || kind == BackgroundKind.Spotlight ||
                string.Equals(System.IO.Path.GetFileName(path ?? ""), "TranscodedWallpaper", StringComparison.OrdinalIgnoreCase);
        }

        // What Restore tells the person.
        public static string RestoreMessageId(SavedBackground saved, bool restored)
        {
            if (saved == null) return "settings.wallpaper.nothing-saved";
            if (saved.Kind == BackgroundKind.Slideshow || saved.Kind == BackgroundKind.Spotlight) return "settings.wallpaper.restored-partial";
            return restored ? "settings.wallpaper.restored" : "settings.wallpaper.restore-failed";
        }

        // WallpaperStyle and TileWallpaper of HKCU\Control Panel\Desktop and the position of IDesktopWallpaper.
        public static int PositionOf(string style, string tile)
        {
            switch ((style ?? "").Trim())
            {
                case "0": return (tile ?? "").Trim() == "1" ? PositionTile : PositionCenter;
                case "2": return PositionStretch;
                case "6": return PositionFit;
                case "22": return PositionSpan;
                default: return PositionFill;
            }
        }

        public static string StyleOf(int position)
        {
            switch (position)
            {
                case PositionCenter: case PositionTile: return "0";
                case PositionStretch: return "2";
                case PositionFit: return "6";
                case PositionSpan: return "22";
                default: return "10";
            }
        }

        public static bool SamePath(string a, string b)
        {
            if (string.IsNullOrEmpty(a) || string.IsNullOrEmpty(b)) return false;
            try { return string.Equals(System.IO.Path.GetFullPath(a), System.IO.Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase); }
            catch (Exception) { return false; }
        }

        // ---- this account and this PC --------------------------------------------------------------------------

        static string Folder()
        {
            return System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm", "wallpaper");
        }

        public static string PicturePath() { return System.IO.Path.Combine(Folder(), "nagi-wallpaper.jpg"); }

        // The container next to this application (the installer copies payload\control, which holds it).
        public static string SourcePath() { return System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, NagiWallpaperFile.Name); }

        // The machine side of Decide: the installer's record (read only) and its state file.
        public static WallpaperDue Check(AppPrefs prefs, out string stamp)
        {
            object choice = null;
            string installed = null, phase = null, package = null;
            stamp = null;
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(ReleaseKey))
                    if (k != null) { choice = k.GetValue(ChoiceValue); stamp = k.GetValue(StampValue) as string; installed = k.GetValue("Version") as string; }
            }
            catch (Exception) { }
            try
            {
                var state = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "installer", "state.json");
                var f = new DriverFacts();
                if (File.Exists(state) && DriverCard.ParseState(File.ReadAllText(state), f)) { phase = f.Phase; package = f.StatePackageVersion; }
            }
            catch (Exception) { }
            return Decide(choice, stamp, prefs.WallpaperApplied, phase, package, installed);
        }

        // An elevated copy of this application may belong to another account (a standard user who typed an
        // administrator's password): the automatic step runs only without elevation. A PC without UAC has no split
        // token and counts as not elevated.
        public static bool ElevatedByUac()
        {
            try
            {
                using (var id = WindowsIdentity.GetCurrent())
                {
                    int type, length;
                    return GetTokenInformation(id.Token, TokenElevationType, out type, 4, out length) && type == TokenElevationTypeFull;
                }
            }
            catch (Exception) { return false; }
        }

        // The sign-in run: waits for the verification of the install, then sets the picture once for its stamp.
        public static void RunAtSignIn(AppPrefs prefs)
        {
            try
            {
                if (ElevatedByUac()) return;
                var clock = Stopwatch.StartNew();
                while (true)
                {
                    string stamp;
                    var due = Check(prefs, out stamp);
                    if (due == WallpaperDue.Due) { SetForStamp(prefs, stamp, "sign-in"); return; }
                    if (due != WallpaperDue.NotVerified || clock.Elapsed.TotalSeconds >= SignInWaitSeconds) return;
                    System.Threading.Thread.Sleep(SignInPollMs);
                }
            }
            catch (Exception e) { Log("sign-in: " + e.Message); }
        }

        // The window's run: once, without waiting.
        public static void RunIfDue(AppPrefs prefs)
        {
            try
            {
                if (ElevatedByUac()) return;
                string stamp;
                if (Check(prefs, out stamp) == WallpaperDue.Due) SetForStamp(prefs, stamp, "window");
            }
            catch (Exception e) { Log("window: " + e.Message); }
        }

        // The stamp is recorded first: one attempt per install, so a picture Windows refuses is not tried again at
        // every start. Settings offers the same step by hand.
        static void SetForStamp(AppPrefs prefs, string stamp, string by)
        {
            prefs.WallpaperApplied = stamp;
            var error = Set(prefs);
            Log(error == null ? "set for install " + stamp + " (" + by + ")" : "not set for install " + stamp + " (" + by + "): " + error);
        }

        // Sets the Nagi wallpaper for this account. null, or what went wrong. The background this account had is
        // saved first, unless it is already the Nagi wallpaper.
        public static string Set(AppPrefs prefs)
        {
            try
            {
                var source = SourcePath();
                if (!File.Exists(source)) return "no " + NagiWallpaperFile.Name + " next to the application";
                string problem;
                var plain = NagiWallpaperFile.Decode(File.ReadAllBytes(source), out problem);
                if (plain == null) return NagiWallpaperFile.Name + ": " + problem;
                var picture = PicturePath();
                Directory.CreateDirectory(Folder());
                var now = ReadCurrent();
                if (!SamePath(now.Path, picture)) prefs.WallpaperPrevious = Keep(now);
                var tmp = picture + ".tmp";
                File.WriteAllBytes(tmp, plain);
                if (File.Exists(picture)) File.Delete(picture);
                File.Move(tmp, picture);
                return SetPicture(picture, PositionFill) ? null : "Windows did not take the picture";
            }
            catch (Exception e) { return e.Message; }
        }

        // Puts back the saved background. The id of the sentence to show.
        public static string Restore(AppPrefs prefs)
        {
            var saved = prefs.WallpaperPrevious;
            bool done = false;
            try
            {
                if (saved != null)
                {
                    if (saved.Kind == BackgroundKind.Solid) done = SetSolid(saved.Color);
                    else if (File.Exists(saved.Path) && !SamePath(saved.Path, PicturePath())) done = SetPicture(saved.Path, PositionOf(saved.Style, saved.Tile));
                }
                Log("restore: " + (saved == null ? "nothing saved" : saved.Kind.ToString().ToLowerInvariant() + (done ? ", done" : ", not done")));
            }
            catch (Exception e) { Log("restore: " + e.Message); done = false; }
            return RestoreMessageId(saved, done);
        }

        static SavedBackground ReadCurrent()
        {
            var s = new SavedBackground();
            var path = new StringBuilder(1024);
            if (SystemParametersInfo(SpiGetDeskWallpaper, (uint)path.Capacity, path, 0)) s.Path = path.ToString();
            object type = null;
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(@"Control Panel\Desktop"))
                    if (k != null) { s.Style = k.GetValue("WallpaperStyle") as string ?? ""; s.Tile = k.GetValue("TileWallpaper") as string ?? ""; }
                using (var k = Registry.CurrentUser.OpenSubKey(@"Control Panel\Colors"))
                    if (k != null) s.Color = k.GetValue("Background") as string ?? "";
                using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers"))
                    if (k != null) type = k.GetValue("BackgroundType");
            }
            catch (Exception) { }
            s.Kind = KindOf(type, s.Path);
            return s;
        }

        // A picture in a cache that the new wallpaper overwrites is copied next to ours first.
        static SavedBackground Keep(SavedBackground s)
        {
            if (!KeepsCopy(s.Kind, s.Path) || !File.Exists(s.Path)) return s;
            var copy = System.IO.Path.Combine(Folder(), "previous-wallpaper" + (System.IO.Path.HasExtension(s.Path) ? System.IO.Path.GetExtension(s.Path) : ".jpg"));
            File.Copy(s.Path, copy, true);
            s.Path = copy;
            return s;
        }

        static bool SetPicture(string path, int position)
        {
            try
            {
                var type = Type.GetTypeFromCLSID(new Guid("C2CF3110-460E-4fc1-B9D0-8A1C0C9CC4BD"));
                var o = Activator.CreateInstance(type);
                try
                {
                    var dw = (IDesktopWallpaper)o;
                    dw.SetPosition(position);
                    dw.SetWallpaper(null, path);
                    return true;
                }
                finally { Marshal.ReleaseComObject(o); }
            }
            catch (Exception) { }
            // Without the shell's object: the two values of the position and the wallpaper itself.
            using (var k = Registry.CurrentUser.CreateSubKey(@"Control Panel\Desktop"))
            {
                k.SetValue("WallpaperStyle", StyleOf(position), RegistryValueKind.String);
                k.SetValue("TileWallpaper", position == PositionTile ? "1" : "0", RegistryValueKind.String);
            }
            return SystemParametersInfo(SpiSetDeskWallpaper, 0, path, SpifUpdateIniFile | SpifSendChange);
        }

        static bool SetSolid(string color)
        {
            var parts = (color ?? "").Split(' ');
            int r, g, b;
            if (parts.Length == 3 && int.TryParse(parts[0], out r) && int.TryParse(parts[1], out g) && int.TryParse(parts[2], out b) &&
                r >= 0 && r < 256 && g >= 0 && g < 256 && b >= 0 && b < 256)
            {
                SetSysColors(1, new[] { ColorDesktop }, new[] { r | g << 8 | b << 16 });
                using (var k = Registry.CurrentUser.CreateSubKey(@"Control Panel\Colors")) k.SetValue("Background", color, RegistryValueKind.String);
            }
            return SystemParametersInfo(SpiSetDeskWallpaper, 0, "", SpifUpdateIniFile | SpifSendChange);
        }

        // One line per attempt in this person's own folder; the file stays small.
        public static void Log(string line)
        {
            try
            {
                Directory.CreateDirectory(Folder());
                var log = System.IO.Path.Combine(Folder(), "wallpaper.log");
                if (File.Exists(log) && new FileInfo(log).Length > 64 * 1024) File.Delete(log);
                File.AppendAllText(log, DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ") + " " + line + Environment.NewLine);
            }
            catch (Exception) { }
        }

        const uint SpiGetDeskWallpaper = 0x0073, SpiSetDeskWallpaper = 0x0014, SpifUpdateIniFile = 0x01, SpifSendChange = 0x02;
        const int ColorDesktop = 1, TokenElevationType = 18, TokenElevationTypeFull = 2;

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool SystemParametersInfo(uint action, uint param, StringBuilder value, uint winIni);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool SystemParametersInfo(uint action, uint param, string value, uint winIni);

        [DllImport("user32.dll", SetLastError = true)]
        static extern bool SetSysColors(int count, int[] elements, int[] colors);

        [DllImport("advapi32.dll", SetLastError = true)]
        static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returned);

        // The shell's desktop wallpaper object (shobjidl_core.h). Only the first eight methods are declared; the
        // order is the order of the interface, which is what a call through it depends on.
        [ComImport, Guid("B92B56A9-8B55-4E14-9A89-0199BBB6F93B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IDesktopWallpaper
        {
            void SetWallpaper([MarshalAs(UnmanagedType.LPWStr)] string monitorId, [MarshalAs(UnmanagedType.LPWStr)] string wallpaper);
            [return: MarshalAs(UnmanagedType.LPWStr)] string GetWallpaper([MarshalAs(UnmanagedType.LPWStr)] string monitorId);
            [return: MarshalAs(UnmanagedType.LPWStr)] string GetMonitorDevicePathAt(uint monitorIndex);
            uint GetMonitorDevicePathCount();
            // Takes a RECT*; declared with a pointer because this application never calls it.
            void GetMonitorRect([MarshalAs(UnmanagedType.LPWStr)] string monitorId, IntPtr rect);
            void SetBackgroundColor(uint color);
            uint GetBackgroundColor();
            void SetPosition(int position);
        }
    }
}
