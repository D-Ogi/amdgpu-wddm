// Host tests of the driver's own switches of the Graphics page (src/DriverFeatures.cs): the contract with the state
// catalog and the KMD source, when a switch is shown, the one write of each action, the refusals, the plain sentences
// and the rule that "on" removes the person's value, so that the installation default (1) is in force again.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static RecoverySnapshot FeatureSnapshot(uint? compute, uint? gdi, uint computeValue = 1, uint gdiValue = 1)
    {
        var s = Open();
        s.Parameters["EnableComputeQueue"] = computeValue;
        s.Parameters["EnableGdiHwAccel"] = gdiValue;
        s.Features = new Dictionary<string, long>();
        if (compute != null) s.Features["ComputeQueue"] = compute.Value;
        if (gdi != null) s.Features["GdiHwAccel"] = gdi.Value;
        return s;
    }

    static void DriverFeatureTests(string root)
    {
        Strings.Language = "en";

        // ---- the contract with the state catalog and the KMD ----------------------------------------------------
        var catalog = File.ReadAllText(Path.Combine(root, @"driver\state\catalog.json"));
        foreach (var f in DriverFeatures.All)
        {
            var setting = Regex.Match(catalog, "\\{\\s*\"path\": \"\",\\s*\"name\": \"" + f.ValueName + "\",(?<b>[^{}]*)\\}");
            Check(setting.Success, "the catalog has the setting " + f.ValueName);
            var b = setting.Groups["b"].Value;
            Check(b.Contains("\"kind\": \"setting\"") && b.Contains("\"type\": \"dword\"") && b.Contains("\"access\": \"admin\"") &&
                b.Contains("\"apply\": \"next-start\""), f.ValueName + ": a DWORD setting an administrator writes, read at the next start");
            Check(b.Contains("\"factory\": 1,") && b.Contains("\"code\": 1,"), f.ValueName + ": on by default, in the INF and in the code");
            var record = Regex.Match(catalog, "\\{\\s*\"path\": \"" + DriverFeatures.FeaturePath + "\",\\s*\"name\": \"" + f.FeatureName + "\",(?<b>[^{}]*)\\}");
            Check(record.Success, "the catalog has the record " + DriverFeatures.FeaturePath + "\\" + f.FeatureName);
            var r = record.Groups["b"].Value;
            Check(r.Contains("\"kind\": \"volatile\"") && r.Contains("\"access\": \"driver\"") && r.Contains("\"readable\": \"user\""),
                f.FeatureName + ": a volatile record the driver writes and every user reads");
            Check(Recovery.Allowed(Recovery.ParametersPath, f.ValueName), "the allow-list has " + f.ValueName);
            Check(!Recovery.Allowed(Recovery.ParametersPath, f.FeatureName), "the allow-list does not have the record " + f.FeatureName);
            foreach (var a in new[] { f.ActionOn, f.ActionOff })
            {
                Check(Recovery.Actions.Contains(a), "the action list has " + a);
                Check(Strings.Has("plan.title." + a) && Strings.Has("plan.line." + a), a + " has its dialog title and its sentence");
            }
            Check(Strings.Has("graphics." + f.Id) && Strings.Has("graphics." + f.Id + ".help") && Strings.Has("help.setting." + f.Id),
                f.Id + ": the check box, its line and its explanation have their texts");
            Check(SettingsSearch.Index.Any(e => e.Id == "graphics." + f.Id && e.Page == "graphics" && !e.ComingLater), f.Id + " is searchable on the Graphics page");
        }
        Check(DriverFeatures.Actions.All(a => Recovery.Actions.Contains(a)) && DriverFeatures.Actions.Length == 4, "four actions, all on the list");
        var gfx = File.ReadAllText(Path.Combine(root, @"driver\kmd\gfx.c"));
        var wddm = File.ReadAllText(Path.Combine(root, @"driver\kmd\wddm.c"));
        Check(gfx.Contains("GuardReadSetting(L\"EnableComputeQueue\", 1)"), "the KMD reads EnableComputeQueue with the default 1");
        Check(wddm.Contains("GuardVolatileStore(L\"Features\", L\"ComputeQueue\", feature)"), "the KMD writes Features\\ComputeQueue");
        Check(!gfx.Contains("EnableComputeNode") && !File.ReadAllText(Path.Combine(root, @"driver\kmd\guard.c")).Contains("EnableComputeNode"),
            "the lab selector EnableComputeNode is gone: the setting is the one switch");

        // ---- when a switch is shown ---------------------------------------------------------------------------
        var none = Open();
        foreach (var f in DriverFeatures.All) Check(!DriverFeatures.Shown(none, f), f.Id + ": no record (an older driver) shows nothing");
        var zero = FeatureSnapshot(0, 0);
        foreach (var f in DriverFeatures.All) Check(!DriverFeatures.Shown(zero, f), f.Id + ": record 0 (not possible on this start) shows nothing");
        var odd = FeatureSnapshot(3, 7);
        foreach (var f in DriverFeatures.All) Check(!DriverFeatures.Shown(odd, f), f.Id + ": an unknown record value shows nothing");
        var on = FeatureSnapshot(1, 1);
        var offBySetting = FeatureSnapshot(2, 2, 0, 0);
        foreach (var f in DriverFeatures.All)
        {
            Check(DriverFeatures.Shown(on, f) && DriverFeatures.Shown(offBySetting, f), f.Id + ": records 1 and 2 show the switch");
            Check(!DriverFeatures.Pending(on, f) && !DriverFeatures.Pending(offBySetting, f), f.Id + ": the choice and the start agree: nothing waits");
        }
        Check(DriverFeatures.Shown(FeatureSnapshot(1, null), DriverFeatures.Compute) && !DriverFeatures.Shown(FeatureSnapshot(1, null), DriverFeatures.Gdi),
            "each switch has its own record: compute without GDI programs shows only the compute switch");
        Check(DriverFeatures.Pending(FeatureSnapshot(1, 1, 0, 1), DriverFeatures.Compute), "turned off, still on in this start: waits for the restart");
        Check(DriverFeatures.Pending(FeatureSnapshot(2, 2, 1, 1), DriverFeatures.Gdi), "turned on, still off in this start: waits for the restart");
        Check(DriverFeatures.On(null) && DriverFeatures.On(1) && DriverFeatures.On(5) && !DriverFeatures.On(0), "only the exact value 0 is off");

        // ---- the plans --------------------------------------------------------------------------------------------
        foreach (var f in DriverFeatures.All)
        {
            var off = Recovery.Plan(f.ActionOff, on);
            Check(!off.Refused && Writes(off, f.ValueName + "=0"), f.ActionOff + " writes " + f.ValueName + " 0");
            Check(off.OfferRestart && off.Undoable && off.Effect == "at the next restart of Windows", f.ActionOff + ": at the next restart, undoable");
            var back = Recovery.Plan(f.ActionOn, offBySetting);
            Check(!back.Refused && Writes(back, f.ValueName + "-"), f.ActionOn + " removes the person's value, so the installation default is in force");
            Check(back.OfferRestart, f.ActionOn + " offers the restart");
            Check(Recovery.Plan(f.ActionOn, on).Refused && PlainPlan.Refusal(Recovery.Plan(f.ActionOn, on)) == Strings.T("graphics.feature.refuse.on-already"),
                f.ActionOn + " refused when it is on already");
            Check(Recovery.Plan(f.ActionOff, offBySetting).Refused && PlainPlan.Refusal(Recovery.Plan(f.ActionOff, offBySetting)) == Strings.T("graphics.feature.refuse.off-already"),
                f.ActionOff + " refused when it is off already");
            foreach (var s in new[] { none, zero, odd })
            {
                var p = Recovery.Plan(f.ActionOff, s);
                Check(p.Refused && p.Writes.Count == 0 && PlainPlan.Refusal(p) == Strings.T("graphics.feature.refuse.unavailable"),
                    f.ActionOff + " refused where the driver cannot use the switch");
            }
            // A choice that waits for its restart can be taken back before the restart.
            var waiting = f == DriverFeatures.Compute ? FeatureSnapshot(1, 1, 0, 1) : FeatureSnapshot(1, 1, 1, 0);
            Check(Writes(Recovery.Plan(f.ActionOn, waiting), f.ValueName + "-"), f.ActionOn + " takes back an off that waits for the restart");
            var d = PlainPlan.Describe(off);
            Equal(Strings.T("plan.title." + f.ActionOff), d.Title, f.ActionOff + ": the dialog title");
            Check(d.Changes.SequenceEqual(new[] { Strings.T("plan.line." + f.ActionOff) }), f.ActionOff + ": one sentence, not two");
            Check(d.Restart && d.Notes.Contains(Strings.T("plan.when.restart")), f.ActionOff + ": the dialog says that it applies after a restart");
            foreach (var t in new[] { d.Title }.Concat(d.Changes).Concat(d.Notes))
                foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: " + f.ActionOff + " dialog: " + x);
            Check(PlainWords.Findings(new[] { f.ValueName }).Any(), "G-NOINT knows the internal name " + f.ValueName);
        }

        // ---- "Restore defaults" puts both switches back to the installation default ---------------------------
        var reset = FeatureSnapshot(2, 2, 0, 0);
        reset.StateSchema = 2;
        reset.ParameterOverrides = new Dictionary<string, long> { { "EnableComputeQueue", 0 }, { "EnableGdiHwAccel", 0 } };
        reset.DefaultParameters = new Dictionary<string, long> { { "EnableGpuPresentBlit", 1 }, { "EnableCddDwmInterop", 1 }, { "DpmMode", 1 }, { "DpmMaxMHz", 1500 },
            { "EnableComputeQueue", 1 }, { "EnableGdiHwAccel", 1 } };
        reset.DefaultRouter = new Dictionary<string, long> { { "DwmForceCpu", 0 } };
        var rp = Recovery.Plan("reset-defaults", reset);
        Check(!rp.Refused, "reset-defaults is planned: " + rp.Refusal);
        foreach (var f in DriverFeatures.All)
            Check(rp.Writes.Count(w => w.Name == f.ValueName && w.Delete) == 1, "reset-defaults removes " + f.ValueName + " once");
        Check(!rp.Notes.Any(n => n.StartsWith("The other ")), "reset-defaults counts no switch of its own as left to the installer");
        var untouched = FeatureSnapshot(1, 1);
        untouched.StateSchema = 2;
        untouched.ParameterOverrides = new Dictionary<string, long>();
        untouched.DefaultParameters = reset.DefaultParameters;
        untouched.DefaultRouter = reset.DefaultRouter;
        untouched.Parameters.Remove("EnableComputeQueue");
        untouched.Parameters.Remove("EnableGdiHwAccel");
        var up = Recovery.Plan("reset-defaults", untouched);
        Check(!up.Refused && !up.Writes.Any(w => DriverFeatures.ValueNames.Contains(w.Name)), "reset-defaults has no step for a switch at its default");

        // ---- the window's texts in every language name no internals -------------------------------------------
        foreach (var lang in Strings.Languages)
            foreach (var f in DriverFeatures.All)
                foreach (var id in new[] { "graphics." + f.Id, "graphics." + f.Id + ".help", "help.setting." + f.Id, "plan.title." + f.ActionOn, "plan.title." + f.ActionOff,
                    "plan.line." + f.ActionOn, "plan.line." + f.ActionOff, "graphics.feature.pending", "graphics.features.title" })
                    foreach (var x in PlainWords.Findings(new[] { Strings.In(lang, id) })) Check(false, "G-NOINT: " + lang + " " + id + ": " + x);
    }
}
