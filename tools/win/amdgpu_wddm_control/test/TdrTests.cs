// Host tests of the waiting time for the graphics (src/TdrSetting.cs, the tdr-delay action and the reset): the range
// and the clamp, the choice list, the one write, the refusals, the plain sentence of the write, and the rule that the
// chosen number is written and never removed, so that the next installation keeps what the person chose (BD-079).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void TdrTests(string root)
    {
        Strings.Language = "en";

        // The contract against Microsoft's own documentation, as the repository's copy of it states the value.
        Equal(@"SYSTEM\CurrentControlSet\Control\GraphicsDrivers", TdrSetting.RegistryPath, "the value lives under the Windows graphics drivers key");
        Equal("TdrDelay", TdrSetting.ValueName, "the value name");
        Equal(2u, TdrSetting.WindowsDefault, "Windows waits 2 seconds when nothing is stored");
        Equal(10u, TdrSetting.ReleaseDefault, "this release writes 10 seconds");
        var doc = Path.Combine(root, @"..\ref\windows-driver-docs\windows-driver-docs-pr\display\tdr-registry-keys.md");
        if (File.Exists(doc))
        {
            var text = File.ReadAllText(doc);
            var block = Regex.Match(text, @"### TdrDelay(?<b>.*?)### TdrDdiDelay", RegexOptions.Singleline).Groups["b"].Value;
            Check(block.Length > 0, "the documentation has a TdrDelay section");
            Check(block.Contains(@"HKEY_LOCAL_MACHINE\System\CurrentControlSet\Control\GraphicsDrivers"), "the documented key is the one this app writes");
            Check(block.Contains("REG_DWORD"), "the documented type is REG_DWORD");
            Check(Regex.IsMatch(block, @"default value is " + TdrSetting.WindowsDefault + " seconds"), "the documented default is " + TdrSetting.WindowsDefault + " seconds");
        }

        // The range. The documentation gives no limit, so the app's own range is the one that holds.
        Check(TdrSetting.Min == 2 && TdrSetting.Max == 60, "the range is 2 to 60 seconds");
        foreach (var n in TdrSetting.Choices) Check(TdrSetting.IsValid(n), "offered waiting time " + n + " is inside the range");
        Check(TdrSetting.Choices.Contains(TdrSetting.WindowsDefault) && TdrSetting.Choices.Contains(TdrSetting.ReleaseDefault),
            "the list offers both Windows' own waiting time and the release default");
        Check(TdrSetting.Choices.SequenceEqual(TdrSetting.Choices.OrderBy(n => n)), "the list is shortest first");
        foreach (var n in new uint[] { 0, 1, 61, 600, uint.MaxValue }) Check(!TdrSetting.IsValid(n), n + " seconds is outside the range");
        Equal(TdrSetting.Min, TdrSetting.Clamp(0), "0 clamps up to the lowest waiting time");
        Equal(TdrSetting.Min, TdrSetting.Clamp(1), "1 clamps up");
        Equal(TdrSetting.Max, TdrSetting.Clamp(uint.MaxValue), "a huge number clamps down to the highest waiting time");
        Equal(10u, TdrSetting.Clamp(10), "a number inside the range is unchanged");
        Equal(TdrSetting.WindowsDefault, TdrSetting.Effective(null), "nothing stored: Windows' own waiting time is in force");
        Equal(30u, TdrSetting.Effective(30), "a stored waiting time is the one in force");
        Equal(1u, TdrSetting.Effective(1), "a stored number outside the range is reported as it is, not clamped");

        // The allow-list: this value and no other of the Windows key.
        Check(Recovery.Allowed(TdrSetting.RegistryPath, TdrSetting.ValueName), "the allow-list has the waiting time");
        foreach (var n in new[] { "TdrLevel", "TdrDdiDelay", "TdrDebugMode", "TdrLimitCount", "TdrTestMode", "HwSchMode" })
            Check(!Recovery.Allowed(TdrSetting.RegistryPath, n), "the allow-list refuses " + n);
        Check(!Recovery.Allowed(@"SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Scheduler", TdrSetting.ValueName), "the allow-list refuses another key");
        Check(Recovery.Actions.Contains("tdr-delay"), "the action list has tdr-delay");

        // The choice list. Nothing stored: the first entry is the state itself, so picking a number is a real change,
        // including the number Windows uses by itself.
        var absent = TdrSetting.Items(null);
        Check(absent[0].Seconds == null, "nothing stored: the first entry is \"not set\"");
        Equal(TdrSetting.Choices.Length + 1, absent.Count, "nothing stored: one entry per waiting time plus \"not set\"");
        Equal(0, TdrSetting.Selected(absent, null, null), "nothing stored: \"not set\" is selected");
        Equal(absent.FindIndex(x => x.Seconds == 10), TdrSetting.Selected(absent, null, 10), "an unsaved choice is the selected entry");
        var stored10 = TdrSetting.Items(10);
        Check(stored10.All(x => x.Seconds != null), "a stored waiting time: no \"not set\" entry");
        Equal(TdrSetting.Choices.Length, stored10.Count, "a stored waiting time that the app offers adds no entry");
        Equal(stored10.FindIndex(x => x.Seconds == 10), TdrSetting.Selected(stored10, 10, null), "the stored waiting time is selected");
        var stored7 = TdrSetting.Items(7);
        Check(stored7.Any(x => x.Seconds == 7), "a stored waiting time the app does not offer is in the list");
        Check(stored7.Select(x => x.Seconds.Value).SequenceEqual(stored7.Select(x => x.Seconds.Value).OrderBy(n => n)), "the list stays shortest first");
        Equal(stored7.FindIndex(x => x.Seconds == 7), TdrSetting.Selected(stored7, 7, null), "a stored waiting time outside the list is selected");
        foreach (var item in absent.Concat(stored7))
            Check(!item.Text.StartsWith("[", StringComparison.Ordinal) && item.Text.Length > 0, "every entry has text: " + item.Text);
        Check(TdrSetting.Label(2) != TdrSetting.Label(10) && TdrSetting.Label(10) != TdrSetting.Label(20), "each waiting time has its own label");
        foreach (var t in absent.Select(x => x.Text).Concat(new[] { TdrSetting.StateText(null), TdrSetting.StateText(10), TdrSetting.StateText(1) }))
            foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: the waiting time card: " + x);

        // One write, and only when the stored value differs. Never a removal: a removed value reads as absent at the
        // next installation, which writes the release default again and would undo the person's choice.
        var one = TdrSetting.PlanWrites(null, 10);
        Equal(1, one.Count, "nothing stored: one write");
        Check(one[0].Path == TdrSetting.RegistryPath && one[0].Name == TdrSetting.ValueName && one[0].Kind == "DWord" && one[0].Number == 10 && !one[0].Delete,
            "the write is TdrDelay = 10 as a DWORD");
        Equal(0, TdrSetting.PlanWrites(10, 10).Count, "the stored waiting time already: no write");
        Equal(1, TdrSetting.PlanWrites(10, 2).Count, "a shorter waiting time is written, not removed");
        Check(!TdrSetting.PlanWrites(10, 2)[0].Delete, "going back to Windows' own waiting time writes 2, it does not remove the value");
        Throws<ArgumentException>(() => TdrSetting.PlanWrites(null, 1), "a waiting time under the range is refused");
        Throws<ArgumentException>(() => TdrSetting.PlanWrites(null, 61), "a waiting time over the range is refused");

        // The plan.
        var s = Open();
        var p = Recovery.Plan("tdr-delay", s, more: new Recovery.PlanArgs { Tdr = 10 });
        Check(!p.Refused && Writes(p, "TdrDelay=10"), "the plan writes the chosen waiting time");
        Check(p.OfferRestart && p.Effect == "at the next restart of Windows" && p.Undoable, "the plan applies at the next restart of Windows and can be undone");
        Check(p.Change.Contains("10"), "the plan's English sentence names the number: " + p.Change);
        var d = PlainPlan.Describe(p);
        Check(d.Changes.Count == 1 && d.Changes[0].Contains("10"), "the dialog names the new waiting time: " + string.Join(" / ", d.Changes));
        Check(!d.Title.StartsWith("[", StringComparison.Ordinal), "the dialog has a title");
        Check(d.Restart && d.Notes.Any(n => n == Strings.T("plan.when.restart")), "the dialog says that a restart of Windows is needed");
        foreach (var t in new[] { d.Title }.Concat(d.Changes).Concat(d.Notes))
            foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: the waiting time dialog: " + x);
        Check(PlainPlan.LineId(p.Writes[0]) != null, "G-PLAN: the write has a plain sentence");

        var set10 = Open(); set10.TdrDelay = 10;
        Check(Recovery.Plan("tdr-delay", set10, more: new Recovery.PlanArgs { Tdr = 10 }).Refusal.Contains("already"), "the waiting time in force already is refused");
        Check(Recovery.Plan("tdr-delay", set10, more: new Recovery.PlanArgs { Tdr = 10 }).Writes.Count == 0, "a refusal plans no write");
        Check(Recovery.Plan("tdr-delay", Open()).Refusal.Contains("No waiting time given"), "no waiting time given is refused");
        Check(Recovery.Plan("tdr-delay", Open(), more: new Recovery.PlanArgs { Tdr = 1 }).Refusal.Contains("between"), "a waiting time outside the range is refused by the plan");
        Check(Recovery.Plan("tdr-delay", Open(), more: new Recovery.PlanArgs { Tdr = 600 }).Refusal.Contains("between"), "a long waiting time is refused by the plan");
        var unreadable = Open(); unreadable.TdrError = "the value has registry type Binary";
        Check(Recovery.Plan("tdr-delay", unreadable, more: new Recovery.PlanArgs { Tdr = 10 }).Refusal.Contains("cannot be read"), "an unreadable waiting time is not overwritten");
        Check(Recovery.Plan("tdr-delay", new RecoverySnapshot(), more: new Recovery.PlanArgs { Tdr = 10 }).Refused, "without the driver there is nothing to change");
        foreach (var bad in new[] { 1u, 61u, 600u })
            Check(!PlainPlan.Refusal(Recovery.Plan("tdr-delay", Open(), more: new Recovery.PlanArgs { Tdr = bad })).StartsWith("[", StringComparison.Ordinal),
                "WU-061: the refusal of " + bad + " seconds has a plain text");

        // A number this app does not offer, stored by hand: it is shown, and a choice from the list replaces it.
        var odd = Open(); odd.TdrDelay = 7;
        Check(TdrSetting.StateText(odd.Tdr()).Contains("7"), "a stored waiting time outside the list is named");
        Check(Writes(Recovery.Plan("tdr-delay", odd, more: new Recovery.PlanArgs { Tdr = 20 }), "TdrDelay=20"), "a choice replaces a waiting time stored by hand");
        var huge = Open(); huge.TdrDelay = (long)uint.MaxValue + 1;
        Equal(null, huge.Tdr(), "a number outside the DWORD range reads as nothing stored");

        // The reset to the release defaults takes the waiting time from the installed release's own table, and only
        // from there: a release that names none, or names one outside the range, leaves the value alone.
        var reset = ResetFixture();
        reset.DefaultGraphicsDrivers = new Dictionary<string, long> { { TdrSetting.ValueName, 10 } };
        var rp = Recovery.Plan("reset-defaults", reset, more: new Recovery.PlanArgs { Games = "keep" });
        Check(!rp.Refused && rp.Writes.Any(w => w.Name == TdrSetting.ValueName && w.Number == 10), "the reset writes the release's waiting time: " + rp.Refusal);
        reset.DefaultGraphicsDrivers = null;
        Check(!Recovery.Plan("reset-defaults", reset, more: new Recovery.PlanArgs { Games = "keep" }).Writes.Any(w => w.Name == TdrSetting.ValueName),
            "a release without a waiting time in its table: the reset leaves the value alone");
        reset.DefaultGraphicsDrivers = new Dictionary<string, long> { { TdrSetting.ValueName, 600 } };
        Check(!Recovery.Plan("reset-defaults", reset, more: new Recovery.PlanArgs { Games = "keep" }).Writes.Any(w => w.Name == TdrSetting.ValueName),
            "a waiting time outside the range is not written by the reset either");
        reset.DefaultGraphicsDrivers = new Dictionary<string, long> { { TdrSetting.ValueName, 10 } };
        reset.TdrDelay = 10;
        Check(!Recovery.Plan("reset-defaults", reset, more: new Recovery.PlanArgs { Games = "keep" }).Writes.Any(w => w.Name == TdrSetting.ValueName),
            "the release's waiting time is in force already: the reset writes nothing for it");
        reset.TdrDelay = null; reset.TdrError = "unreadable";
        Check(!Recovery.Plan("reset-defaults", reset, more: new Recovery.PlanArgs { Games = "keep" }).Writes.Any(w => w.Name == TdrSetting.ValueName),
            "an unreadable waiting time is not written by the reset");

        // The manifest group the installer and the reset share.
        var m = ManifestCheck.Parse("{\"schema\":1,\"defaults\":{\"parameters\":{\"DpmMode\":1},\"desktop_router\":{\"DwmForceCpu\":0},\"graphics_drivers\":{\"TdrDelay\":10}}}");
        Equal(10L, m.DefaultGraphicsDrivers[TdrSetting.ValueName], "the manifest's graphics_drivers group is read");
        Equal(null, ManifestCheck.Parse("{\"schema\":1,\"defaults\":{\"parameters\":{\"DpmMode\":1},\"desktop_router\":{\"DwmForceCpu\":0}}}").DefaultGraphicsDrivers,
            "a manifest without the group reads as none");

        // The state line of the support view and the dry run.
        Func<RecoverySnapshot, StateLine> tdrLine = x => Recovery.Describe(x).First(l => l.Topic == "Graphics waiting time");
        var shortWait = Open(); shortWait.DefaultGraphicsDrivers = new Dictionary<string, long> { { TdrSetting.ValueName, 10 } };
        Equal("warn", tdrLine(shortWait).Severity, "nothing stored while the release wants a longer wait: a warning");
        shortWait.TdrDelay = 2;
        Equal("warn", tdrLine(shortWait).Severity, "a waiting time shorter than the release's: a warning");
        shortWait.TdrDelay = 10;
        Equal("ok", tdrLine(shortWait).Severity, "the release's waiting time: all good");
        shortWait.TdrDelay = null; shortWait.TdrError = "unreadable";
        Equal("warn", tdrLine(shortWait).Severity, "an unreadable waiting time: a warning");

        // The search entry and its explanation (WU-064 checks every entry; this names the one).
        Check(SettingsSearch.Index.Any(e => e.Id == "help.tdr" && e.Page == "help"), "the waiting time is searchable on the Help page");
        Check(Strings.Has("help.setting.tdr") && Strings.Has("tdr.title") && Strings.Has("tdr.apply"), "the card and its explanation have their texts");
        Check(SettingsSearch.Find("freeze", "en").Any(h => h.Entry.Id == "help.tdr"), "a search for a frozen picture finds the waiting time");
        Check(SettingsSearch.Find("waiting time", "en").Any(h => h.Entry.Id == "help.tdr"), "a search for the waiting time finds it");

        // The source rule: nothing but the card reads or writes the value by name, and the window never writes it.
        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        foreach (var f in Directory.GetFiles(src, "MainForm*.cs").Concat(new[] { Path.Combine(src, "RecoveryView.cs") }))
        {
            var text = Regex.Replace(File.ReadAllText(f), @"//[^\n]*", "");
            Check(!text.Contains("\"TdrDelay\""), "G-SRC: " + Path.GetFileName(f) + " does not name the value itself");
        }
    }

    // A snapshot the reset accepts: the driver runs, the release's table is there, and the stored values differ from it.
    static RecoverySnapshot ResetFixture()
    {
        var s = Open();
        s.DefaultParameters = new Dictionary<string, long>
        {
            { "EnableGpuPresentBlit", 1 }, { "EnableCddDwmInterop", 1 }, { "DpmMode", 1 }, { "DpmMaxMHz", 1500 },
        };
        s.DefaultRouter = new Dictionary<string, long> { { "DwmForceCpu", 0 } };
        s.DefaultApplications = new Dictionary<string, string>();
        return s;
    }
}
