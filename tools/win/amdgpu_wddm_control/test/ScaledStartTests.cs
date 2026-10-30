// Host tests of the full-resolution start (src/ScaledStart.cs, BD-196): the detection rule for each of its inputs, the
// EDID native mode, the parsing of bcdedit /enum, the plan of the action (what it writes in each state of the boot
// settings, and every refusal), the undo of it from a backup, the arguments a backup can and cannot reach, the
// calibration of the words bcdedit prints in another language, and the texts in every language. No bcdedit runs here:
// FakeBcd answers as bcdedit does, keeps a store of its own and records every command line.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    // bcdedit as seen through its command line: the system store's {globalsettings}, the calibration stores, and the
    // words of one language for a boolean.
    sealed class FakeBcd : IBcdRunner
    {
        public string Yes = "Yes", No = "No", Header = "Global Settings", Created = "The entry {0} was successfully created.";
        public bool EnumFails, CreateStoreFails;
        public readonly Dictionary<string, string> System = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public readonly Dictionary<string, Dictionary<string, string>> Stores = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        public readonly List<string> Commands = new List<string>();
        public readonly List<string> Removed = new List<string>();
        public const string ProbeId = "{8a3c1f20-5d41-4b6e-9e8f-0c2d7a9b1e44}";

        static BcdRun Ok(string text) { return new BcdRun { Exit = 0, Output = text }; }
        static BcdRun Fail(string text) { return new BcdRun { Exit = 1, Output = text }; }

        static string Print(string header, string id, Dictionary<string, string> elements)
        {
            var w = new StringBuilder();
            w.Append("\r\n" + header + "\r\n" + new string('-', header.Length) + "\r\n");
            w.Append("identifier".PadRight(24) + id + "\r\n");
            w.Append("inherit".PadRight(24) + "{dbgsettings}\r\n" + new string(' ', 24) + "{emssettings}\r\n" + new string(' ', 24) + "{badmemory}\r\n");
            foreach (var e in elements) w.Append(e.Key.PadRight(24) + e.Value + "\r\n");
            return w.ToString();
        }

        public BcdRun Run(string a)
        {
            Commands.Add(a);
            Match m;
            if (a == "/enum {globalsettings}")
                return EnumFails ? Fail("The boot configuration data store could not be opened.\r\nAccess is denied.\r\n") : Ok(Print(Header, "{globalsettings}", System));
            if ((m = Regex.Match(a, @"^/set \{globalsettings\} (?<n>highestmode|graphicsresolution) (?<v>\S+)$")).Success)
            {
                System[m.Groups["n"].Value] = m.Groups["n"].Value == "highestmode" ? (m.Groups["v"].Value == "on" ? Yes : No) : m.Groups["v"].Value;
                return Ok("The operation completed successfully.\r\n");
            }
            if ((m = Regex.Match(a, @"^/deletevalue \{globalsettings\} (?<n>\S+)$")).Success)
                return System.Remove(m.Groups["n"].Value) ? Ok("The operation completed successfully.\r\n") : Fail("An error occurred while attempting to delete the specified data element.\r\nElement not found.\r\n");
            if ((m = Regex.Match(a, "^/createstore \"(?<p>[^\"]+)\"$")).Success)
            {
                if (CreateStoreFails) return Fail("The store creation operation has failed.\r\n");
                Stores[m.Groups["p"].Value] = null;
                return Ok("The operation completed successfully.\r\n");
            }
            if ((m = Regex.Match(a, "^/store \"(?<p>[^\"]+)\" /create /d \"[^\"]+\" /application osloader$")).Success && Stores.ContainsKey(m.Groups["p"].Value))
            {
                Stores[m.Groups["p"].Value] = new Dictionary<string, string>();
                return Ok(string.Format(Created, ProbeId) + "\r\n");
            }
            if ((m = Regex.Match(a, "^/store \"(?<p>[^\"]+)\" /set (?<id>\\{[^}]+\\}) highestmode (?<v>on|off)$")).Success && m.Groups["id"].Value == ProbeId && Stores[m.Groups["p"].Value] != null)
            {
                Stores[m.Groups["p"].Value]["highestmode"] = m.Groups["v"].Value == "on" ? Yes : No;
                return Ok("The operation completed successfully.\r\n");
            }
            if ((m = Regex.Match(a, "^/store \"(?<p>[^\"]+)\" /enum (?<id>\\{[^}]+\\})$")).Success && m.Groups["id"].Value == ProbeId && Stores[m.Groups["p"].Value] != null)
                return Ok(Print("Windows Boot Loader", ProbeId, Stores[m.Groups["p"].Value]));
            return Fail("The parameter is incorrect.\r\n");
        }

        public void RemoveStore(string path) { Removed.Add(path); Stores.Remove(path); }
    }

    const string ProbePath = @"X:\temp\amdgpu-wddm-bcd-probe-test.dat";

    static RecoverySnapshot BootSnapshot(BcdGlobalSettings bcd)
    {
        var s = Open();
        s.Bcd = bcd;
        return s;
    }

    static List<DisplayChoice> ModeList(params int[] whHz)
    {
        var list = new List<DisplayChoice>();
        for (int i = 0; i + 2 < whHz.Length; i += 3) list.Add(new DisplayChoice { Width = whHz[i], Height = whHz[i + 1], RefreshHz = whHz[i + 2] });
        return DisplayInfo.Distinct(list);
    }

    // A valid EDID base block whose first detailed timing has these active pixels.
    static byte[] Edid(int width, int height, bool interlaced = false, int length = 128)
    {
        var e = new byte[length];
        new byte[] { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 }.CopyTo(e, 0);
        e[18] = 1; e[19] = 4;                                 // EDID 1.4
        e[54] = 0x56; e[55] = 0x5E;                           // 241.50 MHz pixel clock
        e[56] = (byte)(width & 0xFF); e[57] = 0xA0; e[58] = (byte)(((width >> 8) & 0x0F) << 4);
        e[59] = (byte)(height & 0xFF); e[60] = 0x29; e[61] = (byte)(((height >> 8) & 0x0F) << 4);
        e[71] = (byte)(interlaced ? 0x9A : 0x1A);
        int sum = 0;
        for (int i = 0; i < 127; i++) sum += e[i];
        e[127] = (byte)((256 - (sum & 0xFF)) & 0xFF);
        return e;
    }

    static string WritesText(ActionPlan p) { return string.Join("; ", p.Writes.Select(w => w.ToString())); }

    static void ScaledStartTests(string root)
    {
        Strings.Language = "en";
        const string set = "bcdedit /set {globalsettings} highestmode on", drop = "bcdedit /deletevalue {globalsettings} graphicsresolution";

        // ---- the detection rule, one input at a time ---------------------------------------------------------------
        var now = new DisplayMode { Device = @"\\.\DISPLAY1", Width = 800, Height = 600, RefreshHz = 60 };
        var native = new Size(2560, 1440);
        var one = ModeList(800, 600, 60);
        Equal(ScaledSignal.OneModeBelowNative, ScaledStart.Detect(now, one, native, null), "one mode, the current one, below the native mode: the scaled start");
        Equal(ScaledSignal.OneModeBelowNative, ScaledStart.Detect(now, ModeList(800, 600, 60, 800, 600, 75), native, null), "one resolution at two rates is still one mode");
        Equal(ScaledSignal.None, ScaledStart.Detect(now, ModeList(800, 600, 60, 640, 480, 60), native, null), "two resolutions: Windows offers a choice, no signal");
        Equal(ScaledSignal.None, ScaledStart.Detect(now, ModeList(1024, 768, 60), native, null), "the one listed mode is not the current one: no signal");
        Equal(ScaledSignal.None, ScaledStart.Detect(now, one, new Size(800, 600), null), "the current mode is the native mode: no signal");
        Equal(ScaledSignal.None, ScaledStart.Detect(new DisplayMode { Width = 1920, Height = 1200 }, ModeList(1920, 1200, 60), new Size(1920, 1080), null),
            "taller than the native mode: not a smaller mode, no signal");
        Equal(ScaledSignal.OneModeBelowNative, ScaledStart.Detect(new DisplayMode { Width = 1920, Height = 1080 }, ModeList(1920, 1080, 60), new Size(2560, 1080), null),
            "narrower at the same height: smaller, the scaled start");
        Equal(ScaledSignal.None, ScaledStart.Detect(now, one, null, null), "no EDID: no signal");
        Equal(ScaledSignal.None, ScaledStart.Detect(now, new List<DisplayChoice>(), native, null), "no listed mode: no signal");
        Equal(ScaledSignal.None, ScaledStart.Detect(null, one, native, null), "no current mode: no signal");
        var fixedStart = new BcdGlobalSettings { GraphicsResolution = "800x600" };
        Equal(ScaledSignal.FixedStartResolution, ScaledStart.Detect(now, ModeList(2560, 1440, 60, 1920, 1080, 60), native, fixedStart), "a fixed start resolution in the boot settings: the signal, whatever the modes");
        Equal(ScaledSignal.FixedStartResolution, ScaledStart.Detect(null, null, null, fixedStart), "a fixed start resolution needs no display reading");
        Equal(ScaledSignal.None, ScaledStart.Detect(now, ModeList(2560, 1440, 60, 1920, 1080, 60), native, new BcdGlobalSettings { HighestMode = "on" }), "highestmode alone is no signal");
        Equal(ScaledSignal.OneModeBelowNative, ScaledStart.Detect(now, one, native, new BcdGlobalSettings { Error = "access denied", GraphicsResolution = "800x600" }),
            "an unread store gives no boot-settings signal; the display rule still holds");

        // ---- the native mode of the EDID ---------------------------------------------------------------------------
        Equal((Size?)new Size(2560, 1440), ScaledStart.NativeSize(Edid(2560, 1440)), "EDID: the first detailed timing, 2560 x 1440");
        Equal((Size?)new Size(1920, 1080), ScaledStart.NativeSize(Edid(1920, 1080, false, 256)), "EDID: a block with an extension, 1920 x 1080");
        Equal((Size?)new Size(3840, 2160), ScaledStart.NativeSize(Edid(3840, 2160)), "EDID: the upper four bits of both sizes, 3840 x 2160");
        var bad = Edid(2560, 1440); bad[100] ^= 1;
        Equal((Size?)null, ScaledStart.NativeSize(bad), "EDID: a wrong checksum");
        bad = Edid(2560, 1440); bad[0] = 1;
        Equal((Size?)null, ScaledStart.NativeSize(bad), "EDID: a wrong header");
        bad = Edid(2560, 1440); bad[54] = 0; bad[55] = 0; bad[127] = 0; bad[127] = (byte)((256 - (bad.Take(127).Sum(x => (int)x) & 0xFF)) & 0xFF);
        Equal((Size?)null, ScaledStart.NativeSize(bad), "EDID: no detailed timing in the first descriptor");
        Equal((Size?)null, ScaledStart.NativeSize(Edid(1920, 540, true)), "EDID: an interlaced timing is not taken");
        Equal((Size?)null, ScaledStart.NativeSize(new byte[64]), "EDID: a short block");
        Equal((Size?)null, ScaledStart.NativeSize(null), "EDID: no value");

        // ---- the parsing of bcdedit /enum ----------------------------------------------------------------------
        var e = ScaledStart.Elements("\r\nGlobal Settings\r\n---------------\r\nidentifier              {globalsettings}\r\ninherit                 {dbgsettings}\r\n" +
            "                        {emssettings}\r\nhighestmode             Yes\r\ngraphicsresolution      800x600\r\n");
        Equal("Yes", e["highestmode"], "parse: highestmode");
        Equal("800x600", e["graphicsresolution"], "parse: graphicsresolution");
        Equal("{dbgsettings}", e["inherit"], "parse: a continued value keeps its first line, the rest is not an element");
        Check(!e.ContainsKey("Global") && !e.ContainsKey("---------------") && !e.ContainsKey("{emssettings}"), "parse: the header, the rule and the continuation are not elements");
        Equal("Tak", ScaledStart.Elements("Ustawienia globalne\r\n-------------------\r\nhighestmode             Tak\r\n")["highestmode"], "parse: a translated value as printed");
        Equal(FakeBcd.ProbeId, ScaledStart.FirstGuid("Wpis " + FakeBcd.ProbeId + " został pomyślnie utworzony."), "parse: the new entry's id in a translated message");

        // ---- the plan, in each state of the boot settings ------------------------------------------------------
        Check(Recovery.Actions.Contains(ScaledStart.Action), "the action list has " + ScaledStart.Action);
        Equal(0u, BoardCapabilities.ActionPermission(ScaledStart.Action), "the action needs no board permission: it is not board-specific");
        var p = Recovery.Plan(ScaledStart.Action, BootSnapshot(null));
        Check(!p.Refused && WritesText(p) == set, "window (store not read): highestmode on: " + p.Refusal + WritesText(p));
        Check(p.OfferRestart && p.Undoable && p.Effect == "at the next restart of Windows", "the plan offers the restart and is undoable");
        Check(p.Notes.Any(n => n.Contains("elevated helper")), "window: the note says that the helper reads the store and plans again");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings()));
        Equal(set, WritesText(p), "nothing set: highestmode on only");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { HighestMode = "off", HighestModeText = "No" }));
        Equal(set, WritesText(p), "highestmode off: set it on");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { GraphicsResolution = "800x600" }));
        Equal(set + "; " + drop, WritesText(p), "a fixed start resolution: highestmode on, then graphicsresolution removed");
        Check(p.Writes[0].Path == ScaledStart.BcdPath && p.Writes[0].Kind == "Bcd" && p.Writes[1].Delete, "the writes are boot settings, not registry values");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { HighestMode = "on", HighestModeText = "Yes", GraphicsResolution = "1024x768" }));
        Equal(drop, WritesText(p), "highestmode on already, a fixed resolution: only the removal");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { HighestMode = "on", HighestModeText = "Yes" }));
        Check(p.Refused && p.Writes.Count == 0 && PlainPlan.Refusal(p) == Strings.T("plan.full-res.refuse.already"), "everything set already: refused, restart to use it");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { HighestMode = "unknown", HighestModeText = "Vielleicht" }));
        Check(p.Refused && p.Refusal.Contains("Vielleicht") && PlainPlan.Refusal(p) == Strings.T("plan.full-res.refuse.unreadable"), "a word the app cannot match: refused, an undo could not put it back");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { Error = "bcdedit /enum {globalsettings}: exit 1: Access is denied." }));
        Check(p.Refused && p.Refusal.Contains("Access is denied") && PlainPlan.Refusal(p) == Strings.T("plan.full-res.refuse.unreadable"), "a store that cannot be read: refused");
        p = Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { GraphicsResolution = "800x600 & calc" }));
        Check(p.Refused && p.Writes.Count == 0, "a start resolution the app cannot write back is not removed");
        Check(Recovery.Plan(ScaledStart.Action, new RecoverySnapshot()).Refused, "without the driver there is nothing to change");
        Check(ScaledStart.Shown(Open()) && !ScaledStart.Shown(new RecoverySnapshot()) && !ScaledStart.Shown(null), "the window offers the action wherever the driver is installed");

        // ---- the undo from a backup ----------------------------------------------------------------------------
        Func<BackupValue[], BackupRecord> backup = values => new BackupRecord { File = "backup-20261030T120000000Z.json", Action = ScaledStart.Action, Utc = "2026-10-30T12:00:00.0000000Z", Undoable = true, Values = values.ToList() };
        var u = Recovery.Plan("undo", Open(), null, null, new[] { backup(new[] {
            new BackupValue { Path = ScaledStart.BcdPath, Name = "highestmode", Existed = false },
            new BackupValue { Path = ScaledStart.BcdPath, Name = "graphicsresolution", Existed = true, Kind = "Bcd", Text = "800x600" } }) });
        Equal("bcdedit /deletevalue {globalsettings} highestmode; bcdedit /set {globalsettings} graphicsresolution 800x600", WritesText(u), "undo removes highestmode and puts the start resolution back");
        Check(!u.Refused && u.OfferRestart && u.Effect == "at the next restart of Windows", "undo: at the next restart");
        u = Recovery.Plan("undo", Open(), null, null, new[] { backup(new[] { new BackupValue { Path = ScaledStart.BcdPath, Name = "highestmode", Existed = true, Kind = "Bcd", Text = "off" } }) });
        Equal("bcdedit /set {globalsettings} highestmode off", WritesText(u), "undo of a stored off writes off");
        foreach (var evil in new[] {
            new BackupValue { Path = ScaledStart.BcdPath, Name = "graphicsresolution", Existed = true, Kind = "Bcd", Text = "800x600 /set {bootmgr} timeout 0" },
            new BackupValue { Path = ScaledStart.BcdPath, Name = "highestmode", Existed = true, Kind = "Bcd", Text = "maybe" },
            new BackupValue { Path = ScaledStart.BcdPath, Name = "highestmode", Existed = true, Kind = "String", Text = "on" },
            new BackupValue { Path = ScaledStart.BcdPath, Name = "bootmenupolicy", Existed = true, Kind = "Bcd", Text = "legacy" },
            new BackupValue { Path = "BCD {bootmgr}", Name = "highestmode", Existed = false } })
            Check(Recovery.Plan("undo", Open(), null, null, new[] { backup(new[] { evil }) }).Refused, "undo refuses a backup value this app does not write: " + evil.Name + " " + evil.Text);
        var ud = PlainPlan.Describe(Recovery.Plan("undo", Open(), null, null, new[] { backup(new[] { new BackupValue { Path = ScaledStart.BcdPath, Name = "highestmode", Existed = false } }) }));
        Check(ud.Changes.SequenceEqual(new[] { Strings.T("plan.line.full-resolution-start-undo") }), "the undo dialog has the sentence of the way back");

        // ---- the arguments: only the two elements of {globalsettings}, only the values the app writes ----------
        Equal("/set {globalsettings} highestmode on", ScaledStart.Arguments(ScaledStart.Set("highestmode", "on")), "arguments: highestmode on");
        Equal("/set {globalsettings} graphicsresolution 1024x768", ScaledStart.Arguments(ScaledStart.Set("graphicsresolution", "1024x768")), "arguments: a start resolution");
        Equal("/deletevalue {globalsettings} graphicsresolution", ScaledStart.Arguments(ScaledStart.Remove("graphicsresolution")), "arguments: a removal");
        foreach (var w in new[] { ScaledStart.Set("highestmode", "yes"), ScaledStart.Set("graphicsresolution", "800x600;x"), ScaledStart.Set("graphicsresolution", "x"),
            ScaledStart.Set("timeout", "0"), new RegWrite { Path = "BCD {bootmgr}", Name = "highestmode", Kind = "Bcd", Text = "on" }, RegWrite.Dword(Recovery.ParametersPath, "highestmode", 1) })
            Throws<ArgumentException>(() => ScaledStart.Arguments(w), "arguments refused: " + w.Path + " " + w.Name + " " + w.Text);
        Check(!Recovery.Allowed(ScaledStart.BcdPath, "timeout") && !Recovery.Allowed("BCD {bootmgr}", "highestmode") && Recovery.Allowed(ScaledStart.BcdPath, "graphicsresolution"),
            "the allow-list has the two elements of {globalsettings} only");
        Check(!DriverStateKey.IsStateValue(ScaledStart.BcdPath, "highestmode") && !GraphicsSettings.Owns(ScaledStart.Set("highestmode", "on")), "a boot setting goes neither to the state client nor to the graphics settings");

        // ---- BcdEdit over a fake bcdedit: reading, the words of another language, the writes, the read-back ----
        var english = new FakeBcd();
        english.System["highestmode"] = "Yes";
        var r = new BcdEdit(english, () => ProbePath).Read();
        Check(r.Error == null && r.HighestMode == "on" && r.HighestModeText == "Yes" && r.GraphicsResolution == null, "English: highestmode Yes reads as on");
        Check(english.Removed.SequenceEqual(new[] { ProbePath }), "the calibration store is removed after the calibration");

        var polish = new FakeBcd { Yes = "Tak", No = "Nie", Header = "Ustawienia globalne", Created = "Wpis {0} został pomyślnie utworzony." };
        polish.System["highestmode"] = "Nie";
        polish.System["graphicsresolution"] = "800x600";
        var boot = new BcdEdit(polish, () => ProbePath);
        r = boot.Read();
        Check(r.HighestMode == "off" && r.GraphicsResolution == "800x600", "Polish: Nie reads as off after the calibration: " + ScaledStart.StatusText(r));
        Check(polish.Commands.Count(c => c.StartsWith("/createstore", StringComparison.Ordinal)) == 1, "one calibration per run");
        boot.Read();
        Check(polish.Commands.Count(c => c.StartsWith("/createstore", StringComparison.Ordinal)) == 1, "a second reading keeps the words it learned");

        // The full round of the helper on the Polish store: plan, backup, writes with read-back, then the undo.
        var before = new Dictionary<string, string>(polish.System);
        var plan = Recovery.Plan(ScaledStart.Action, BootSnapshot(boot.Read()));
        Equal(set + "; " + drop, WritesText(plan), "Polish store: both writes");
        var saved = plan.Writes.Select(w => boot.Saved(w.Name)).ToArray();
        Check(saved[0].Existed && saved[0].Kind == "Bcd" && saved[0].Text == "off" && saved[1].Existed && saved[1].Text == "800x600", "the backup holds off and 800x600, not the words of the language");
        foreach (var w in plan.Writes) { boot.Write(w); Check(boot.ReadsBack(w), "read back: " + w); }
        Check(polish.System["highestmode"] == "Tak" && !polish.System.ContainsKey("graphicsresolution"), "the store has highestmode on and no start resolution");
        Check(Recovery.Plan(ScaledStart.Action, BootSnapshot(boot.Read())).Refused, "after the change the action has nothing to do");
        var undo = Recovery.Plan("undo", Open(), null, null, new[] { backup(saved) });
        Equal("bcdedit /set {globalsettings} highestmode off; bcdedit /set {globalsettings} graphicsresolution 800x600", WritesText(undo), "the undo of the round");
        foreach (var w in undo.Writes) { boot.Write(w); Check(boot.ReadsBack(w), "undo read back: " + w); }
        Check(polish.System.Count == before.Count && before.All(kv => polish.System[kv.Key] == kv.Value), "the undo gives the store as it was");

        // A removal of an element that is not there runs no bcdedit write; a refused write runs nothing.
        boot.Write(ScaledStart.Remove("highestmode"));
        Check(!polish.System.ContainsKey("highestmode"), "a removal removes the element");
        int count = polish.Commands.Count;
        boot.Write(ScaledStart.Remove("highestmode"));
        Check(!polish.Commands.Skip(count).Any(c => c.StartsWith("/deletevalue", StringComparison.Ordinal)), "a second removal has nothing to do");
        count = polish.Commands.Count;
        Throws<ArgumentException>(() => boot.Write(ScaledStart.Set("highestmode", "Tak")), "a write of another value is refused");
        Equal(count, polish.Commands.Count, "the refused write ran no bcdedit");
        // Nothing but {globalsettings} and the two elements in the system store; the calibration names its own file.
        foreach (var c in polish.Commands.Concat(english.Commands))
            Check(Regex.IsMatch(c, @"^/(enum \{globalsettings\}|set \{globalsettings\} (highestmode (on|off)|graphicsresolution [0-9]+x[0-9]+)|deletevalue \{globalsettings\} (highestmode|graphicsresolution))$") ||
                c.StartsWith("/createstore \"" + ProbePath + "\"", StringComparison.Ordinal) || c.StartsWith("/store \"" + ProbePath + "\" ", StringComparison.Ordinal),
                "only the two elements of {globalsettings} and the calibration store: " + c);

        // The calibration fails: an English word still reads, a translated one does not, and the plan refuses then.
        var noProbe = new FakeBcd { Yes = "Tak", No = "Nie", CreateStoreFails = true };
        noProbe.System["highestmode"] = "Tak";
        r = new BcdEdit(noProbe, () => ProbePath).Read();
        Check(r.HighestMode == "unknown" && r.HighestModeText == "Tak", "no calibration: a translated word is unknown");
        Check(noProbe.Removed.SequenceEqual(new[] { ProbePath }), "a failed calibration removes its store too");
        Check(Recovery.Plan(ScaledStart.Action, BootSnapshot(r)).Refused, "no calibration, a translated word: the plan refuses");
        Throws<InvalidOperationException>(() => new BcdEdit(noProbe, () => ProbePath).Saved("highestmode"), "the backup of a word it cannot match stops the action before a write");
        var noProbeEnglish = new FakeBcd { CreateStoreFails = true };
        noProbeEnglish.System["highestmode"] = "No";
        Equal("off", new BcdEdit(noProbeEnglish, () => ProbePath).Read().HighestMode, "no calibration: the English word still reads");
        var absent = new FakeBcd { Yes = "Tak", No = "Nie" };
        r = new BcdEdit(absent, () => ProbePath).Read();
        Check(r.HighestMode == null && !absent.Commands.Any(c => c.StartsWith("/createstore", StringComparison.Ordinal)), "nothing set: no calibration is needed");
        var denied = new FakeBcd { EnumFails = true };
        r = new BcdEdit(denied, () => ProbePath).Read();
        Check(r.Error != null && r.Error.Contains("Access is denied"), "a store that cannot be opened is an error: " + r.Error);
        Throws<InvalidOperationException>(() => new BcdEdit(denied, () => ProbePath).Saved("graphicsresolution"), "no backup without a reading");
        Check(!new BcdEdit(denied, () => ProbePath).ReadsBack(ScaledStart.Set("highestmode", "on")), "no read-back without a reading");

        // ---- the dialog and the texts ----------------------------------------------------------------------------
        var d = PlainPlan.Describe(Recovery.Plan(ScaledStart.Action, BootSnapshot(new BcdGlobalSettings { GraphicsResolution = "800x600" })));
        Equal(Strings.T("plan.title." + ScaledStart.Action), d.Title, "the dialog title");
        Check(d.Changes.SequenceEqual(new[] { Strings.T("plan.line." + ScaledStart.Action) }), "both writes: one sentence, what the person gets");
        Check(d.Restart && d.Notes.Contains(Strings.T("plan.when.restart")) && d.Notes.Contains(Strings.T("plan.undo.yes")), "the dialog says restart, and that it can be undone");
        Equal("plan.line." + ScaledStart.Action, PlainPlan.LineId(ScaledStart.Remove("graphicsresolution")), "the removal of the start resolution is part of the change");
        Equal("plan.line." + ScaledStart.Action + "-undo", PlainPlan.LineId(ScaledStart.Set("graphicsresolution", "800x600")), "a start resolution written back is the way back");
        Equal("plan.line." + ScaledStart.Action + "-undo", PlainPlan.LineId(ScaledStart.Remove("highestmode")), "highestmode removed is the way back");
        Check(PlainPlan.LineId(RegWrite.Dword(Recovery.ParametersPath, "highestmode", 1)) == null, "the name alone on another path has no sentence");
        foreach (var word in new[] { "bcdedit", "highestmode", "graphicsresolution", "{globalsettings}", "EDID" })
            Check(PlainWords.Findings(new[] { "x " + word + " y" }).Any(), "G-NOINT knows " + word);
        var ids = new[] { "display.full-res.small", "display.full-res.fixed", "display.full-res.help", "display.full-res.button", "plan.title." + ScaledStart.Action,
            "plan.line." + ScaledStart.Action, "plan.line." + ScaledStart.Action + "-undo", "plan.full-res.refuse.unreadable", "plan.full-res.refuse.already" };
        foreach (var lang in Strings.Languages)
            foreach (var id in ids)
            {
                Check(!string.IsNullOrEmpty(Strings.In(lang, id)), lang + " " + id + " has a text");
                foreach (var x in PlainWords.Findings(new[] { Strings.In(lang, id) })) Check(false, "G-NOINT: " + lang + " " + id + ": " + x);
            }
        Check(Strings.T("display.full-res.help").Contains("restart"), "the row says that the change works after a restart");
        Check(Strings.T("display.full-res.small", "800 x 600", "2560 x 1440").Contains("2560 x 1440"), "the row names the monitor's own mode");

        // ---- the source rule: bcdedit runs from one place, and the window never names the elements ----------------
        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        foreach (var f in Directory.GetFiles(src, "*.cs"))
        {
            var code = Regex.Replace(File.ReadAllText(f), @"//[^\n]*", "");
            var name = Path.GetFileName(f);
            Check(name == "RecoveryActions.cs" || !code.Contains("bcdedit.exe"), "G-SRC: only RecoveryActions.cs starts bcdedit.exe: " + name);
            if (name.StartsWith("MainForm", StringComparison.Ordinal))
                Check(!code.Contains("\"highestmode\"") && !code.Contains("\"graphicsresolution\"") && !code.Contains("bcdedit"), "G-SRC: " + name + " names no boot setting itself");
        }
        var actionsCode = File.ReadAllText(Path.Combine(src, "RecoveryActions.cs"));
        Check(Regex.Matches(actionsCode, @"""bcdedit\.exe""").Count == 1 && actionsCode.Contains("sealed class BcdeditProcess : IBcdRunner"), "G-SRC: one bcdedit.exe, in BcdeditProcess");
        Check(actionsCode.Contains("if (ScaledStart.Owns(path)) return RecoveryProbe.Boot.Saved(name);") && actionsCode.Contains("if (ScaledStart.Owns(w.Path)) { RecoveryProbe.Boot.Write(w); return; }") &&
            actionsCode.Contains("if (ScaledStart.Owns(w.Path)) return RecoveryProbe.Boot.ReadsBack(w);"), "the helper's backup, write and read-back send a boot setting to BcdEdit");
    }
}
