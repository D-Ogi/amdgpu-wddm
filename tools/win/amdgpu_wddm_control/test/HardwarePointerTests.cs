// Host tests of the hardware mouse pointer switch of the Graphics page (src/HardwarePointerSetting.cs): the contract
// with the KMD source and the state catalog, the raw route past the state client, when the switch is shown, the one
// write of each action, the refusals, the line that shows the stored value, the plain sentences in every language,
// and the rule that "on" removes the value, so that the driver's default (1) is in force again.
using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static RecoverySnapshot PointerSnapshot(long? stored, string error = null)
    {
        var s = Open();
        s.HardwarePointerValue = stored;
        s.HardwarePointerError = error;
        return s;
    }

    static void HardwarePointerTests(string root)
    {
        Strings.Language = "en";
        const string name = HardwarePointerSetting.ValueName;

        // ---- the contract with the KMD and the state catalog ----------------------------------------------------
        var guard = File.ReadAllText(Path.Combine(root, @"driver\kmd\guard.c"));
        Check(guard.Contains("ParameterDword(key, L\"" + name + "\""), "the KMD reads " + name + " from its Parameters key");
        var shape = File.ReadAllText(Path.Combine(root, @"driver\kmd\dcn_cursor_shape.h"));
        Check(Regex.IsMatch(shape, @"#define BC250_CURSOR_SETTING_ON 0x" + HardwarePointerSetting.OnBit.ToString("X") + "u"), "bit 0 is the pointer, as in the KMD");
        var catalog = File.ReadAllText(Path.Combine(root, @"driver\state\catalog.json"));
        Check(!catalog.Contains("\"" + name + "\""), name + " is not in the state catalog, so the app must write it raw");
        Check(string.Equals(HardwarePointerSetting.RegistryPath, Recovery.ParametersPath, StringComparison.Ordinal), "the value lives in the driver's Parameters key");

        // ---- the allow-list and the raw route -----------------------------------------------------------------
        Check(Recovery.Allowed(Recovery.ParametersPath, name), "the allow-list has " + name);
        Check(!Recovery.Allowed(@"SYSTEM\CurrentControlSet\Control\GraphicsDrivers", name), "the allow-list refuses the value under another key");
        Check(!Recovery.ParameterNames.Contains(name), name + " is not on the list of state values");
        Check(!DriverStateKey.IsStateValue(Recovery.ParametersPath, name), name + " does not go through the state client");
        Check(DriverStateKey.IsStateValue(Recovery.ParametersPath, "EnableComputeQueue"), "the other Parameters values still go through the state client");
        Check(!DriverStateKey.IsStateValue(TdrSetting.RegistryPath, TdrSetting.ValueName), "a Windows value is not a state value");
        // Every branch of the helper between the state client and the registry asks about the value, not the key: a
        // branch on the key alone would send the raw value to the state client, which refuses a name it does not know.
        var actions = Regex.Replace(File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\RecoveryActions.cs")), @"//[^\n]*", "");
        Check(!actions.Contains("DriverStateKey.IsPath("), "G-RAW: the helper chooses the route by IsStateValue only");
        Check(Regex.Matches(actions, @"DriverStateKey\.IsStateValue\(").Count >= 4, "G-RAW: the batch, the backup, the write and the read-back use IsStateValue");
        foreach (var a in HardwarePointerSetting.Actions)
        {
            Check(Recovery.Actions.Contains(a), "the action list has " + a);
            Check(Strings.Has("plan.title." + a) && Strings.Has("plan.line." + a), a + " has its dialog title and its sentence");
        }
        Check(SettingsSearch.Index.Any(e => e.Id == "graphics." + HardwarePointerSetting.Id && e.Page == "graphics" && !e.ComingLater),
            "the switch is searchable on the Graphics page");
        Check(SettingsSearch.Find("cursor", "en").Any(h => h.Entry.Id == "graphics." + HardwarePointerSetting.Id), "a search for the cursor finds the switch");

        // ---- the stored value as the driver reads it ---------------------------------------------------------
        Check(HardwarePointerSetting.On(null) && HardwarePointerSetting.On(1) && HardwarePointerSetting.On(3) && HardwarePointerSetting.On(7),
            "absent and every value with bit 0 is on");
        Check(!HardwarePointerSetting.On(0) && !HardwarePointerSetting.On(2) && !HardwarePointerSetting.On(4), "a value without bit 0 is off");
        Equal(null, PointerSnapshot((long)uint.MaxValue + 1).HardwarePointer(), "a number outside the DWORD range reads as nothing stored");

        // ---- when the switch is shown, and what its line says --------------------------------------------------
        Check(HardwarePointerSetting.Shown(Open()), "the switch is shown wherever the driver is installed");
        Check(!HardwarePointerSetting.Shown(new RecoverySnapshot()) && !HardwarePointerSetting.Shown(null), "no driver: no switch");
        Check(DriverFeatures.Searchable(Open(), "graphics." + HardwarePointerSetting.Id) && !DriverFeatures.Searchable(new RecoverySnapshot(), "graphics." + HardwarePointerSetting.Id),
            "the search lists the switch only where the window shows it");
        Equal(Strings.T("graphics.hw-pointer.now.default"), HardwarePointerSetting.StateText(PointerSnapshot(null)), "nothing stored: on, the default");
        Equal(Strings.T("graphics.hw-pointer.now.on"), HardwarePointerSetting.StateText(PointerSnapshot(3)), "a stored value with bit 0: on");
        Equal(Strings.T("graphics.hw-pointer.now.off"), HardwarePointerSetting.StateText(PointerSnapshot(0)), "a stored 0: off");
        Equal(Strings.T("graphics.hw-pointer.unreadable"), HardwarePointerSetting.StateText(PointerSnapshot(null, "has registry type String")), "a value of another type: cannot be read");

        // ---- the plans --------------------------------------------------------------------------------------------
        var off = Recovery.Plan(HardwarePointerSetting.ActionOff, PointerSnapshot(null));
        Check(!off.Refused && Writes(off, name + "=0"), "off writes " + name + " 0: " + off.Refusal);
        Check(off.Writes[0].Path == Recovery.ParametersPath && off.Writes[0].Kind == "DWord", "the write is a DWORD in the Parameters key");
        Check(off.OfferRestart && off.Undoable && off.Effect == "at the next restart of Windows", "off: at the next restart, undoable");
        var on = Recovery.Plan(HardwarePointerSetting.ActionOn, PointerSnapshot(0));
        Check(!on.Refused && Writes(on, name + "-"), "on removes the value, so the driver's default is in force");
        Check(on.OfferRestart, "on offers the restart");
        Check(Writes(Recovery.Plan(HardwarePointerSetting.ActionOff, PointerSnapshot(5)), name + "=0"), "off replaces a lab value with 0");
        var onAlready = Recovery.Plan(HardwarePointerSetting.ActionOn, PointerSnapshot(null));
        Check(onAlready.Refused && onAlready.Writes.Count == 0 && PlainPlan.Refusal(onAlready) == Strings.T("graphics.feature.refuse.on-already"), "on refused when it is on already");
        var offAlready = Recovery.Plan(HardwarePointerSetting.ActionOff, PointerSnapshot(0));
        Check(offAlready.Refused && PlainPlan.Refusal(offAlready) == Strings.T("graphics.feature.refuse.off-already"), "off refused when it is off already");
        var unreadable = Recovery.Plan(HardwarePointerSetting.ActionOff, PointerSnapshot(null, "has registry type String"));
        Check(unreadable.Refused && unreadable.Writes.Count == 0 && PlainPlan.Refusal(unreadable) == Strings.T("graphics.hw-pointer.refuse.unreadable"),
            "a value the app cannot read is not overwritten");
        Check(Recovery.Plan(HardwarePointerSetting.ActionOff, new RecoverySnapshot()).Refused, "without the driver there is nothing to change");
        foreach (var p in new[] { off, on })
        {
            var d = PlainPlan.Describe(p);
            Equal(Strings.T("plan.title." + p.Action), d.Title, p.Action + ": the dialog title");
            Check(d.Changes.SequenceEqual(new[] { Strings.T("plan.line." + p.Action) }), p.Action + ": one sentence, the one of the action");
            Check(d.Restart && d.Notes.Contains(Strings.T("plan.when.restart")), p.Action + ": the dialog says that it applies after a restart");
            Check(PlainPlan.LineId(p.Writes[0]) == "plan.line." + p.Action, "G-PLAN: " + p.Action + " has its plain sentence");
        }
        Equal("plan.line." + HardwarePointerSetting.ActionOn, PlainPlan.LineId(RegWrite.Dword(Recovery.ParametersPath, name, 3)), "an undo to a lab value 3 reads as on");
        Equal("plan.line." + HardwarePointerSetting.ActionOff, PlainPlan.LineId(RegWrite.Dword(Recovery.ParametersPath, name, 2)), "an undo to a value without bit 0 reads as off");
        Check(PlainWords.Findings(new[] { name }).Any(), "G-NOINT knows the internal name " + name);

        // ---- the window's texts in every language name no internals ---------------------------------------------
        var ids = new[] { "graphics.hw-pointer", "graphics.hw-pointer.help", "graphics.hw-pointer.now.default", "graphics.hw-pointer.now.on",
            "graphics.hw-pointer.now.off", "graphics.hw-pointer.unreadable", "graphics.hw-pointer.refuse.unreadable", "help.setting.hw-pointer",
            "plan.title.hw-pointer-on", "plan.title.hw-pointer-off", "plan.line.hw-pointer-on", "plan.line.hw-pointer-off" };
        foreach (var lang in Strings.Languages)
            foreach (var id in ids)
            {
                Check(!string.IsNullOrEmpty(Strings.In(lang, id)), lang + " " + id + " has a text");
                foreach (var x in PlainWords.Findings(new[] { Strings.In(lang, id) })) Check(false, "G-NOINT: " + lang + " " + id + ": " + x);
            }
        Check(Strings.T("graphics.hw-pointer.help").Contains("restart"), "the row says that a change works after a restart");

        // ---- the source rule: nothing but the setting names the value -----------------------------------------
        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        foreach (var f in Directory.GetFiles(src, "MainForm*.cs"))
            Check(!Regex.Replace(File.ReadAllText(f), @"//[^\n]*", "").Contains("\"" + name + "\""), "G-SRC: " + Path.GetFileName(f) + " does not name the value itself");
    }
}
