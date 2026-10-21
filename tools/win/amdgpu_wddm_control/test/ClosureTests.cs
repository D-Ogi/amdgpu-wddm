// BD-181: the rule that tells a closure of the driver's own from a choice of a person, the notice card it feeds,
// and the one action that reverts every such closure. The rule is compared with the release installer's own table
// (tools/release/installer/common.ps1, $script:DriverClosures and Get-DriverClosure), so a value, a record or a
// reason that moves there fails here and not on a tester's PC.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    // The snapshot the lab was left in after the 0x116 of 2026-10-15: both closed, each with the durable record.
    static RecoverySnapshot ClosedByDriver()
    {
        var s = Closed();
        s.Parameters["DpmClosedReason"] = 4;
        s.Parameters["DpmLastReason"] = 1;              // a later start: the live reason only says "fixed requested"
        s.Parameters["InteropLastState"] = 0;
        s.DefaultParameters["DpmMode"] = 1;
        return s;
    }

    static void ClosureTests(string root)
    {
        Strings.Language = "en";

        // ---- the rule against the installer's own table -----------------------------------------------------------
        var common = File.ReadAllText(Path.Combine(root, @"tools\release\installer\common.ps1"));
        foreach (var name in new[] { "EnableGpuPresentBlit", "EnableCddDwmInterop", "DpmMode" })
            Check(Regex.IsMatch(common, @"(?m)^\s*" + name + @" = @\{ closed = 0;"), "common.ps1 closes " + name + " with a 0");
        Check(common.Contains("record = 'InteropClosedReason'") && common.Contains("record = 'DpmClosedReason'") &&
            common.Contains("legacy_record = 'DpmLastReason'"), "common.ps1 names the same records");
        Check(Regex.IsMatch(common, @"DpmMode = @\{[^}]*names = @\{ 3 = 'unconfirmed'; 4 = 'unclean'; 8 = 'smu-error' \}", RegexOptions.Singleline),
            "common.ps1 names the same reasons of a clock fallback");
        // The three names are the ones this app may write at all, and the interop record only ever gets deleted.
        foreach (var name in new[] { "EnableGpuPresentBlit", "EnableCddDwmInterop", "DpmMode", "InteropClosedReason" })
            Check(Recovery.Allowed(Recovery.ParametersPath, name), "the allow-list has " + name);
        Check(!Recovery.Allowed(Recovery.ParametersPath, "DpmClosedReason"), "the clock's record is the driver's: not on the allow-list");

        // ---- the allow-list against the state catalog's access rules ----------------------------------------------
        // The state client refuses a write or a delete that the catalog does not allow (lab, 2026-10-21: the delete
        // of DpmClosedReason, access "driver", failed with Windows error 5 and failed the whole action). A setting on
        // the allow-list must be one an administrator may write, a record one an administrator may delete.
        var access = CatalogAccess(root);
        foreach (var name in Recovery.ParameterNames)
        {
            string[] entry;
            if (!access.TryGetValue(name, out entry)) continue;          // not a catalog value (BootLogo has its own key)
            Check(entry[0] == "setting" ? entry[1] == "admin" : entry[0] == "record" && entry[1] == "admin_clear",
                "catalog allows the app's use of " + name + " (" + entry[0] + ", access " + entry[1] + ")");
        }
        Check(access.ContainsKey("InteropClosedReason") && access.ContainsKey("DpmClosedReason") && access["DpmClosedReason"][1] == "driver",
            "the catalog has both records, and the clock's record is the driver's");
        // The graphics cores stay out: the release default is 24, which is also the value the driver closes with,
        // so Get-DriverClosure returns nothing for them, and the CU values go through the CU setter (PlanTests).
        Check(!Recovery.Allowed(Recovery.ParametersPath, "CuMode") && !Recovery.Allowed(Recovery.ParametersPath, "CuModeClosedReason"),
            "the graphics cores stay outside this action");

        // ---- Find: what counts as a closure of the driver's own ---------------------------------------------------
        var both = ClosureNotice.Find(ClosedByDriver());
        Check(both.Select(c => c.Name).SequenceEqual(new[] { "EnableGpuPresentBlit", "EnableCddDwmInterop", "DpmMode" }),
            "both closures found, desktop first: " + ClosureNotice.Text(both));
        Check(both.All(c => c.Default == 1 && c.Closed == 0 && c.Code == 4), "each closure carries its default and the reason 4");
        Check(both[0].Clear.SequenceEqual(new[] { "InteropClosedReason" }) && both[2].Clear.Length == 0,
            "the desktop closure names the record this reopen deletes; the clock's record stays for the driver");
        Check(ClosureNotice.Subjects(both).SequenceEqual(new[] { ClosureNotice.Desktop, ClosureNotice.Clock }), "two subjects, desktop first");

        // A driver before 0.7.208.1 writes no DpmClosedReason: the legacy record names the clock fallback, and a
        // reopen deletes nothing there, because that record is the reason of the last start.
        var legacy = Closed();                       // DpmLastReason 4, no DpmClosedReason
        var byLegacy = ClosureNotice.Find(legacy).Single(c => c.Name == "DpmMode");
        Check(byLegacy.Record == "DpmLastReason" && byLegacy.Code == 4 && byLegacy.Clear.Length == 0, "the legacy record names the closure and is not deleted");
        foreach (var reason in new uint[] { 1, 2, 5, 9 })
        {
            var other = Closed(); other.Parameters["DpmLastReason"] = reason;
            Check(!ClosureNotice.Find(other).Any(c => c.Name == "DpmMode"), "a legacy reason of " + reason + " is not a clock fallback");
        }
        var durableZero = Closed(); durableZero.Parameters["DpmClosedReason"] = 0; durableZero.Parameters["DpmLastReason"] = 4;
        Check(ClosureNotice.Find(durableZero).Any(c => c.Name == "DpmMode" && c.Record == "DpmLastReason"),
            "a durable record holding nothing does not hide the legacy one");
        var newReason = ClosedByDriver(); newReason.Parameters["DpmClosedReason"] = 77;
        Check(ClosureNotice.Find(newReason).Single(c => c.Name == "DpmMode").Code == 77, "any reason in the durable record is the driver's act");

        // A 0 somebody wrote by hand has no record: it is a choice and nothing here touches it (BD-069, BD-181).
        var chosen = ClosedByDriver();
        chosen.Parameters.Remove("InteropClosedReason"); chosen.Parameters.Remove("DpmClosedReason"); chosen.Parameters["DpmLastReason"] = 1;
        Check(ClosureNotice.Find(chosen).Count == 0 && ClosureNotice.Card(chosen) == null, "a 0 without a record is a choice: no closure, no notice");
        // The record alone is not a closure either: the value has to be the one the driver writes.
        var openAgain = ClosedByDriver();
        openAgain.Parameters["EnableGpuPresentBlit"] = 1; openAgain.Parameters["EnableCddDwmInterop"] = 1; openAgain.Parameters["DpmMode"] = 1;
        Check(ClosureNotice.Find(openAgain).Count == 0, "the records of a reopen that waits for the restart are no closure");

        // A release whose own default is the closed value took nothing away (the graphics-core rule of common.ps1).
        var wantsOff = ClosedByDriver(); wantsOff.DefaultParameters["DpmMode"] = 0;
        Check(!ClosureNotice.Find(wantsOff).Any(c => c.Name == "DpmMode"), "a release default of 0 is not a closure");
        // A default out of range, a name the release does not list, and no defaults at all: nothing is claimed.
        var odd = ClosedByDriver(); odd.DefaultParameters["DpmMode"] = 7;
        Check(!ClosureNotice.Find(odd).Any(c => c.Name == "DpmMode"), "a default out of range is not a closure");
        var unlisted = ClosedByDriver(); unlisted.DefaultParameters.Remove("DpmMode");
        Check(!ClosureNotice.Find(unlisted).Any(c => c.Name == "DpmMode"), "a name the release does not list is not judged");
        var noDefaults = ClosedByDriver(); noDefaults.DefaultParameters = null;
        Check(ClosureNotice.Find(noDefaults).Count == 0 && ClosureNotice.Card(noDefaults) == null, "without the release defaults nothing is claimed");
        var unread = ClosedByDriver(); unread.ReadFailed = true;
        Check(ClosureNotice.Find(unread).Count == 0, "a failed reading claims nothing");
        Check(ClosureNotice.Find(null).Count == 0 && ClosureNotice.Find(new RecoverySnapshot()).Count == 0, "no snapshot and no driver: nothing");

        // ---- the plan ---------------------------------------------------------------------------------------------
        var p = Recovery.Plan(ClosureNotice.ActionName, ClosedByDriver());
        Check(!p.Refused && Writes(p, "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "DpmMode=1", "InteropClosedReason-"),
            "the action writes the defaults first and deletes the interop record after them: " + p.Text());
        Check(!p.Writes.Any(w => w.Name == "DpmClosedReason"), "the clock's record is never in a plan");
        Check(p.OfferRestart && p.Undoable && p.Effect == "at the next restart of Windows" && p.Cu == null && p.Tune == null && p.TuneSteps.Count == 0,
            "the action is undoable, offers the restart and touches nothing else");
        Check(p.Writes.All(w => Recovery.Allowed(w.Path, w.Name)), "the action writes only allowed values");
        Check(p.Notes.Any(n => n.Contains("BD-059")), "the desktop part carries the BD-059 note, like the single reopen");
        // Only the clock closed: only the clock is written, and the desktop note is not there.
        var clockOnly = ClosedByDriver();
        clockOnly.Parameters["EnableGpuPresentBlit"] = 1; clockOnly.Parameters["EnableCddDwmInterop"] = 1; clockOnly.Parameters.Remove("InteropClosedReason");
        p = Recovery.Plan(ClosureNotice.ActionName, clockOnly);
        Check(!p.Refused && Writes(p, "DpmMode=1") && !p.Notes.Any(n => n.Contains("BD-059")), "the clock alone: one value, the record stays for the driver");
        // Only the desktop closed (the state after "Re-enable automatic clocks" alone).
        var desktopOnly = ClosedByDriver(); desktopOnly.Parameters["DpmMode"] = 1; desktopOnly.Parameters.Remove("DpmClosedReason");
        p = Recovery.Plan(ClosureNotice.ActionName, desktopOnly);
        Check(!p.Refused && Writes(p, "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "InteropClosedReason-"), "the desktop alone: both switches and one record");
        // A closure the driver left without its record is reopened, and nothing is deleted that is not there.
        var noMark = ClosedByDriver(); noMark.Parameters.Remove("DpmClosedReason"); noMark.Parameters["DpmLastReason"] = 4;
        Check(Writes(Recovery.Plan(ClosureNotice.ActionName, noMark), "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "DpmMode=1", "InteropClosedReason-"),
            "the legacy record of the clock is left where it is");

        // Refusals.
        Equal("The driver has closed nothing by itself: there is nothing to reopen.",
            Recovery.Plan(ClosureNotice.ActionName, OpenAndRunning()).Refusal, "refused when nothing is closed");
        Check(Recovery.Plan(ClosureNotice.ActionName, chosen).Refusal.StartsWith("The driver has closed nothing", StringComparison.Ordinal),
            "refused when the zeros are a choice of the person");
        Check(Recovery.Plan(ClosureNotice.ActionName, noDefaults).Refusal.StartsWith("The installed release has no list of default settings", StringComparison.Ordinal),
            "refused without the release defaults");
        Check(Recovery.Plan(ClosureNotice.ActionName, new RecoverySnapshot()).Refused, "refused without the driver");
        foreach (var refused in new[] { Recovery.Plan(ClosureNotice.ActionName, chosen), Recovery.Plan(ClosureNotice.ActionName, noDefaults) })
            Check(refused.Writes.Count == 0 && !PlainPlan.Refusal(refused).StartsWith("[", StringComparison.Ordinal), "a refusal writes nothing and has plain words");

        // ---- the dialog and the notice card -----------------------------------------------------------------------
        p = Recovery.Plan(ClosureNotice.ActionName, ClosedByDriver());
        var d = PlainPlan.Describe(p);
        Check(d.Changes.Count == 3 && d.Changes.Contains(Strings.T("plan.line.desktop-on")) && d.Changes.Contains(Strings.T("plan.line.auto-on")) &&
            d.Changes.Contains(Strings.T("plan.line.clear-mark")), "the dialog says it in three sentences: " + string.Join(" / ", d.Changes));
        Check(d.Restart && d.Notes.Contains(Strings.T("plan.dark-screen")), "the dialog offers the restart and warns about the dark screen");
        foreach (var lang in Strings.Languages)
        {
            Strings.Language = lang;
            var card = ClosureNotice.Card(ClosedByDriver());
            Check(card != null && card.Items.Count == 2 && card.Action == ClosureNotice.ActionName && card.StatusId == "gpu-desktop-closed",
                lang + ": the card has both lines and the one button");
            foreach (var t in card.Texts)
            {
                Check(!string.IsNullOrEmpty(t) && !t.StartsWith("[", StringComparison.Ordinal), lang + ": the card's text exists: " + t);
                foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: " + lang + " notice card: " + x);
            }
            foreach (var x in PlainWords.Findings(PlainPlan.Describe(Recovery.Plan(ClosureNotice.ActionName, ClosedByDriver())).Changes))
                Check(false, "G-NOINT: " + lang + " dialog: " + x);
            Check(!Strings.T("status.see-notice").StartsWith("[", StringComparison.Ordinal) &&
                !Strings.T("status.clock-closed").StartsWith("[", StringComparison.Ordinal), lang + ": the status lines of the notice exist");
        }
        Strings.Language = "en";
        Check(ClosureNotice.Card(clockOnly).StatusId == "clock-closed", "the clock alone carries its own status line");

        // ---- Home ------------------------------------------------------------------------------------------------
        var home = HomeStatus.Compute(new StatusInputs { Snapshot = ClosedByDriver() });
        Check(home.Notice != null && home.Severity == "attention", "Home: the notice is there and the card asks for attention");
        var closedItems = home.Items.Where(i => i.Cause == GuideCause.GpuDesktopClosed).ToList();
        Check(closedItems.Count == 1 && closedItems[0].Hidden && closedItems[0].Action == ClosureNotice.ActionName,
            "Home: one hidden item carries the verdict and the one action");
        Check(!home.Items.Any(i => !i.Hidden && (i.Cause == GuideCause.GpuDesktopClosed || i.Action == "reopen-gpu-path")),
            "Home: nothing of this state is said twice");
        var noNotice = HomeStatus.Compute(new StatusInputs { Snapshot = noDefaults });
        Check(noNotice.Notice == null && noNotice.Items.Any(i => i.Action == "reopen-gpu-path" && !i.Hidden),
            "Home: without the release defaults the single line and its own button stay");
        Check(HomeStatus.Compute(new StatusInputs { Snapshot = chosen }).Notice == null, "Home: a choice of the person gets no notice");
        var fine = HomeStatus.Compute(new StatusInputs { Snapshot = OpenAndRunning() });
        Check(fine.Notice == null && !fine.Items.Any(i => i.Cause == GuideCause.GpuDesktopClosed), "Home: nothing closed, no notice");
    }

    // Kind and access of every value at the root of the driver's settings (path ""), from driver/state/catalog.json.
    // The entries are flat objects, written by the catalog generator in a fixed key order, so a pattern reads them.
    static Dictionary<string, string[]> CatalogAccess(string root)
    {
        var text = File.ReadAllText(Path.Combine(root, @"driver\state\catalog.json"));
        var found = new Dictionary<string, string[]>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(text,
            "\"path\":\\s*\"(?<p>[^\"]*)\",\\s*\"name\":\\s*\"(?<n>[^\"]+)\",\\s*\"kind\":\\s*\"(?<k>[^\"]+)\"[^{}]*?\"access\":\\s*\"(?<a>[^\"]+)\""))
            if (m.Groups["p"].Value.Length == 0) found[m.Groups["n"].Value] = new[] { m.Groups["k"].Value, m.Groups["a"].Value };
        Check(found.Count > 100, "the catalog pattern reads the catalog: " + found.Count + " root values");
        return found;
    }

    // The interop switches open and the clock asked for: nothing for this action to do.
    static RecoverySnapshot OpenAndRunning()
    {
        var s = Open();
        s.Parameters["DpmMode"] = 1; s.Parameters.Remove("DpmClosedReason"); s.Parameters["DpmLastReason"] = 0;
        return s;
    }
}
