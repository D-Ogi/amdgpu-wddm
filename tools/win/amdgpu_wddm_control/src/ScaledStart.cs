// BD-196: a scaled start of the screen. When the firmware starts a monitor in a small mode that the monitor stretches
// (for example 800 x 600 on a 2560 x 1440 panel), the driver keeps the mode it inherited and Windows lists that one
// mode only. The Windows boot manager can start the screen in the highest mode of the firmware instead: the boolean
// element highestmode of the {globalsettings} object of the boot configuration (BCD) store ("bcdedit /set
// {globalsettings} highestmode on", devtest/bcdedit--set.md: "Enables boot applications to use the highest graphical
// mode exposed by the firmware"). A graphicsresolution element in the same object fixes the start mode (bcdedit--set.md:
// "Defines the graphics resolution, 1024x768, 800x600, 1024x600, etc."), and the lab makes the bad start with it
// (bcdedit /set {globalsettings} graphicsresolution 800x600), so the action removes it as well.
//
// What the action may change, and nothing more: those two elements of {globalsettings}, through bcdedit. The BCD store
// is a file on the system partition that Windows reads, not a firmware variable, so this is not an NVRAM write, and no
// boot entry, no other object and no other element is touched. The calibration below writes only to a store file of
// its own in the temporary folder, never to the system store.
//
// Every device: the elements belong to the Windows boot manager and the firmware's GOP mode list, not to the BC-250
// board, so the action has no board permission (BoardCapabilities.ActionPermission gives it 0) and runs wherever the
// driver is installed. Board-specific controls are only for a positively identified BC-250 (BoardCapabilities); this
// one is not board-specific. Note: the undo action asks for every board permission, so on another card the change is
// undone with bcdedit by hand.
//
// The detection (Detect) needs no administrator: the current mode and the listed modes come from Windows
// (DisplayInfo), and the monitor's native mode from its EDID, which every user may read. The BCD store needs
// administrator, so only the elevated helper and an elevated --status read it (BcdEdit); the window plans without it,
// and the helper plans again with it.
//
// bcdedit prints a boolean in the language of Windows ("Yes" in English, "Tak" in Polish). BcdEdit learns the two words
// of this Windows from a store of its own: it sets highestmode on, then off, on one entry of a new store file and reads
// back what bcdedit prints. A stored word it cannot match is "unknown", and the plan refuses then, because an undo must
// be able to put back what was there.
//
// Pure apart from the runner: the plan, the parsing and the BCD logic take an IBcdRunner, which the helper gives as
// bcdedit.exe (RecoveryActions.cs, BcdeditProcess) and the unit tests give as a fake store.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    // What the snapshot knows about the boot settings of Windows. Null in the snapshot: not read (no administrator).
    public sealed class BcdGlobalSettings
    {
        public string Error { get; set; }               // null: read
        public string HighestMode { get; set; }         // null: not set; "on", "off" or "unknown"
        public string HighestModeText { get; set; }     // the word bcdedit printed, for the log
        public string GraphicsResolution { get; set; }  // null: not set; else as bcdedit printed it ("800x600")
    }

    public enum ScaledSignal { None, OneModeBelowNative, FixedStartResolution }

    // One bcdedit run: its exit code and what it printed (standard output and standard error).
    public sealed class BcdRun
    {
        public int Exit;
        public string Output;
    }

    public interface IBcdRunner
    {
        BcdRun Run(string arguments);
        void RemoveStore(string path);     // the calibration store file and the log files of its hive
    }

    public static class ScaledStart
    {
        public const string Action = "full-resolution-start";
        // The RegWrite path of the two elements. Not a registry path: Owns() sends every write on it to bcdedit.
        public const string BcdPath = "BCD {globalsettings}";
        public const string Object = "{globalsettings}";
        public const string HighestMode = "highestmode", GraphicsResolution = "graphicsresolution";
        public const string Kind = "Bcd";
        public const string On = "on", Off = "off", Unknown = "unknown";

        static readonly Regex Resolution = new Regex(@"^[1-9][0-9]{2,4}x[1-9][0-9]{2,4}$");

        public static bool Owns(string path) { return string.Equals(path, BcdPath, StringComparison.Ordinal); }

        public static bool Allowed(string path, string name) { return Owns(path) && (name == HighestMode || name == GraphicsResolution); }

        public static bool ValidResolution(string text) { return text != null && Resolution.IsMatch(text); }

        // The window shows the action wherever the driver is installed (see the header: not board-specific).
        public static bool Shown(RecoverySnapshot s) { return s != null && s.DriverInstalled; }

        // ---- the detection ----------------------------------------------------------------------------------------

        // The rule of BD-196: Windows lists exactly one resolution for the monitor, it is the current one, and it is
        // smaller than the native mode of the monitor's EDID; or the boot settings fix a start resolution (known only
        // when they were read). A monitor whose modes or EDID cannot be read gives no signal of the first kind.
        public static ScaledSignal Detect(DisplayMode now, IList<DisplayChoice> modes, Size? native, BcdGlobalSettings bcd)
        {
            if (bcd != null && bcd.Error == null && bcd.GraphicsResolution != null) return ScaledSignal.FixedStartResolution;
            if (now == null || modes == null || native == null) return ScaledSignal.None;
            var sizes = DisplayInfo.Resolutions(modes);
            if (sizes.Count != 1 || sizes[0].Width != now.Width || sizes[0].Height != now.Height) return ScaledSignal.None;
            var n = native.Value;
            bool smaller = now.Width <= n.Width && now.Height <= n.Height && (now.Width < n.Width || now.Height < n.Height);
            return smaller ? ScaledSignal.OneModeBelowNative : ScaledSignal.None;
        }

        // The native mode of a monitor: the first detailed timing descriptor of its EDID base block (VESA E-EDID 1.3 and
        // 1.4: bytes 54-71, the preferred timing mode). Null when the block is short, has a wrong header or checksum,
        // or does not start with a detailed timing (pixel clock 0), or when that timing is interlaced.
        public static Size? NativeSize(byte[] edid)
        {
            if (edid == null || edid.Length < 128) return null;
            var header = new byte[] { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
            for (int i = 0; i < header.Length; i++) if (edid[i] != header[i]) return null;
            int sum = 0;
            for (int i = 0; i < 128; i++) sum += edid[i];
            if ((sum & 0xFF) != 0) return null;
            const int d = 54;
            if (edid[d] == 0 && edid[d + 1] == 0) return null;
            if ((edid[d + 17] & 0x80) != 0) return null;
            int width = edid[d + 2] | ((edid[d + 4] & 0xF0) << 4);
            int height = edid[d + 5] | ((edid[d + 7] & 0xF0) << 4);
            return width > 0 && height > 0 ? new Size(width, height) : (Size?)null;
        }

        // ---- the plan -----------------------------------------------------------------------------------------------

        public static RegWrite Set(string name, string value) { return new RegWrite { Path = BcdPath, Name = name, Kind = Kind, Text = value }; }

        public static RegWrite Remove(string name) { return RegWrite.Remove(BcdPath, name); }

        // Fills the plan of the action. The return value is the refusal, or null when the plan stands.
        public static string Fill(RecoverySnapshot s, ActionPlan p)
        {
            p.Title = "Start the screen in its full resolution";
            p.Change = "the Windows boot manager starts the screen in the highest mode of the firmware, from the next start of Windows";
            p.Effect = "at the next restart of Windows";
            p.Undoable = true;
            var b = s.Bcd;
            if (b == null)
            {
                // The window has no administrator rights, so it cannot read the store. The helper plans again with the
                // store read; both of its writes have the same sentence in the dialog.
                p.Writes.Add(Set(HighestMode, On));
                p.Notes.Add("The boot settings are read by the elevated helper, which also removes graphicsresolution from " + Object + " when it is set.");
            }
            else
            {
                if (b.Error != null)
                {
                    p.PlainRefusal = Strings.T("plan.full-res.refuse.unreadable");
                    return "The boot settings of Windows cannot be read, so they are not changed: " + b.Error;
                }
                if (b.HighestMode == Unknown)
                {
                    p.PlainRefusal = Strings.T("plan.full-res.refuse.unreadable");
                    return HighestMode + " reads \"" + b.HighestModeText + "\", which this app cannot match to on or off, so an undo could not put it back; nothing is changed.";
                }
                if (b.GraphicsResolution != null && !ValidResolution(b.GraphicsResolution))
                {
                    p.PlainRefusal = Strings.T("plan.full-res.refuse.unreadable");
                    return GraphicsResolution + " reads \"" + b.GraphicsResolution + "\", which this app cannot write back, so it is not removed.";
                }
                if (b.HighestMode != On) p.Writes.Add(Set(HighestMode, On));
                if (b.GraphicsResolution != null) p.Writes.Add(Remove(GraphicsResolution));
                if (p.Writes.Count == 0)
                {
                    p.PlainRefusal = Strings.T("plan.full-res.refuse.already");
                    return HighestMode + " is on already and " + Object + " sets no " + GraphicsResolution + ": restart Windows if the screen still starts small.";
                }
            }
            p.OfferRestart = true;
            return null;
        }

        // The bcdedit arguments of one write. Throws ArgumentException for any write this app does not make, so a
        // backup file with another value can never reach the command line.
        public static string Arguments(RegWrite w)
        {
            if (w == null || !Allowed(w.Path, w.Name)) throw new ArgumentException("not a boot setting this app changes");
            if (w.Delete) return "/deletevalue " + Object + " " + w.Name;
            if (w.Kind != Kind) throw new ArgumentException("a boot setting write has kind " + w.Kind);
            if (w.Name == HighestMode && (w.Text == On || w.Text == Off)) return "/set " + Object + " " + HighestMode + " " + w.Text;
            if (w.Name == GraphicsResolution && ValidResolution(w.Text)) return "/set " + Object + " " + GraphicsResolution + " " + w.Text;
            throw new ArgumentException(w.Name + " cannot be set to \"" + w.Text + "\"");
        }

        public static bool Valid(RegWrite w)
        {
            try { Arguments(w); return true; }
            catch (ArgumentException) { return false; }
        }

        // The write as the log, the dry run and the support report show it.
        public static string CommandText(RegWrite w)
        {
            try { return "bcdedit " + Arguments(w); }
            catch (ArgumentException e) { return "bcdedit: refused write of " + w.Name + " (" + e.Message + ")"; }
        }

        // The plain sentence of a write: the change, or the way back from it (an undo).
        public static string LineId(RegWrite w)
        {
            bool forward = w.Name == HighestMode ? !w.Delete && w.Text == On : w.Delete;
            return forward ? "plan.line." + Action : "plan.line." + Action + "-undo";
        }

        public static string StatusText(BcdGlobalSettings b)
        {
            if (b == null) return "not read (needs administrator)";
            if (b.Error != null) return "cannot be read: " + b.Error;
            return Object + " " + HighestMode + " " + (b.HighestMode == null ? "not set" : b.HighestMode == Unknown ? "\"" + b.HighestModeText + "\" (unknown)" : b.HighestMode) +
                ", " + GraphicsResolution + " " + (b.GraphicsResolution ?? "not set");
        }

        // ---- the parsing of bcdedit /enum ---------------------------------------------------------------------------

        // The elements of one object as bcdedit /enum prints them: "name   value" per line, a value that goes on over
        // more lines (inherit lists) continued by lines that start with a space. The names are bcdedit's own and not
        // translated; the values can be.
        public static Dictionary<string, string> Elements(string output)
        {
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var raw in (output ?? "").Replace("\r", "").Split('\n'))
            {
                if (raw.Length == 0 || char.IsWhiteSpace(raw[0])) continue;
                var m = Regex.Match(raw, @"^(?<name>[A-Za-z][A-Za-z0-9]*)\s{2,}(?<value>\S.*)$");
                if (m.Success && !map.ContainsKey(m.Groups["name"].Value)) map[m.Groups["name"].Value] = m.Groups["value"].Value.Trim();
            }
            return map;
        }

        public static string FirstGuid(string output)
        {
            var m = Regex.Match(output ?? "", @"\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}");
            return m.Success ? m.Value : null;
        }

        public static string Short(string text)
        {
            var t = (text ?? "").Replace("\r", " ").Replace("\n", " ").Trim();
            return t.Length > 300 ? t.Substring(0, 300) : t;
        }
    }

    // The two elements of {globalsettings}, read and written through bcdedit (administrator only). One instance per
    // helper run: the words of the calibration are kept for the run.
    public sealed class BcdEdit
    {
        readonly IBcdRunner _run;
        readonly Func<string> _probePath;
        string _on, _off;
        bool _probed;

        public BcdEdit(IBcdRunner run, Func<string> probePath) { _run = run; _probePath = probePath; }

        public BcdGlobalSettings Read()
        {
            var r = _run.Run("/enum " + ScaledStart.Object);
            if (r.Exit != 0) return new BcdGlobalSettings { Error = "bcdedit /enum " + ScaledStart.Object + ": exit " + r.Exit + ": " + ScaledStart.Short(r.Output) };
            var e = ScaledStart.Elements(r.Output);
            var g = new BcdGlobalSettings();
            string text;
            if (e.TryGetValue(ScaledStart.HighestMode, out text)) { g.HighestModeText = text; g.HighestMode = Classify(text); }
            if (e.TryGetValue(ScaledStart.GraphicsResolution, out text)) g.GraphicsResolution = text;
            return g;
        }

        string Classify(string text)
        {
            Probe();
            if (_on != null) return text == _on ? ScaledStart.On : text == _off ? ScaledStart.Off : ScaledStart.Unknown;
            // No calibration: the English words only.
            return text == "Yes" ? ScaledStart.On : text == "No" ? ScaledStart.Off : ScaledStart.Unknown;
        }

        // Learns the words bcdedit prints for on and off, from one Windows Boot Loader entry of a new store file of its
        // own. The system store is never named here. The file and its hive logs are removed at the end.
        void Probe()
        {
            if (_probed) return;
            _probed = true;
            string path = _probePath();
            if (path == null || path.IndexOf('"') >= 0) return;
            string store = "/store \"" + path + "\" ";
            try
            {
                if (_run.Run("/createstore \"" + path + "\"").Exit != 0) return;
                var created = _run.Run(store + "/create /d \"amdgpu-wddm probe\" /application osloader");
                var id = ScaledStart.FirstGuid(created.Output);
                if (created.Exit != 0 || id == null) return;
                string on = ProbeWord(store, id, ScaledStart.On), off = ProbeWord(store, id, ScaledStart.Off);
                if (!string.IsNullOrEmpty(on) && !string.IsNullOrEmpty(off) && on != off) { _on = on; _off = off; }
            }
            finally { _run.RemoveStore(path); }
        }

        string ProbeWord(string store, string id, string value)
        {
            if (_run.Run(store + "/set " + id + " " + ScaledStart.HighestMode + " " + value).Exit != 0) return null;
            var r = _run.Run(store + "/enum " + id);
            string text;
            return r.Exit == 0 && ScaledStart.Elements(r.Output).TryGetValue(ScaledStart.HighestMode, out text) ? text : null;
        }

        // The value of one element as found before an action, for the backup. Throws when it cannot be put back.
        public BackupValue Saved(string name)
        {
            var g = Read();
            if (g.Error != null) throw new InvalidOperationException(g.Error + "; nothing was changed");
            var v = new BackupValue { Path = ScaledStart.BcdPath, Name = name };
            string now = Value(g, name);
            if (now == null) return v;
            if (now == ScaledStart.Unknown || name == ScaledStart.GraphicsResolution && !ScaledStart.ValidResolution(now))
                throw new InvalidOperationException(name + " reads \"" + (name == ScaledStart.HighestMode ? g.HighestModeText : now) + "\", which this app cannot put back; nothing was changed");
            v.Existed = true; v.Kind = ScaledStart.Kind; v.Text = now;
            return v;
        }

        static string Value(BcdGlobalSettings g, string name)
        {
            if (name == ScaledStart.HighestMode) return g.HighestMode;
            if (name == ScaledStart.GraphicsResolution) return g.GraphicsResolution;
            throw new ArgumentException("not a boot setting this app reads: " + name);
        }

        // One write. A removal of an element that is not there has nothing to do (bcdedit would fail on it).
        public void Write(RegWrite w)
        {
            string arguments = ScaledStart.Arguments(w);
            if (w.Delete)
            {
                var g = Read();
                if (g.Error == null && Value(g, w.Name) == null) return;
            }
            var r = _run.Run(arguments);
            if (r.Exit != 0) throw new InvalidOperationException("bcdedit " + arguments + ": exit " + r.Exit + ": " + ScaledStart.Short(r.Output));
        }

        public bool ReadsBack(RegWrite w)
        {
            var g = Read();
            if (g.Error != null) return false;
            string now = Value(g, w.Name);
            return w.Delete ? now == null : now != null && string.Equals(now, w.Text, StringComparison.OrdinalIgnoreCase);
        }
    }
}
