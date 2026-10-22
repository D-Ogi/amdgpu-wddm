// Host tests of the sign-in notice (src/StartNotice.cs), the sign-in script that starts it (start-window.ps1, no
// window of its own since 2026-10-22) and the one-click repair (the enable-driver action of Recovery.cs, with its one
// registry write under a stand-in). No driver, no window, no registry of this PC.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static StartReading Reading(bool running, uint? starts, uint? stage, bool installed = true, bool read = true)
    {
        return new StartReading { Read = read, Installed = installed, Running = running, UnconfirmedStarts = starts, LastStage = stage,
            DriverError = running ? null : "no answer" };
    }

    static void StartNoticeTests(string root)
    {
        // ---- the decision: case A, case B, and every state that shows nothing -----------------------------------
        // Case A is the main case: the guard refused the start and Windows is on its basic
        // display. It is reached by either witness, because the two are written at different moments: the count
        // reaches the limit before the refusing start, and the refusing start records the stage.
        Equal(StartNoticeCase.Guard, StartNotice.Decide(Reading(false, 2, 90)), "case A: the count at the limit and the refusing stage");
        Equal(StartNoticeCase.Guard, StartNotice.Decide(Reading(false, 2, 61)), "case A: the count at the limit alone");
        Equal(StartNoticeCase.Guard, StartNotice.Decide(Reading(false, 0, 90)), "case A: the refusing stage alone (the driver cleared the count)");
        Equal(StartNoticeCase.Guard, StartNotice.Decide(Reading(false, 7, null)), "case A: a count above the limit");
        // Case B: the driver is installed and did not start, and nothing says the guard refused it. There is no
        // one-click repair for this, so the notice only sends the person to the control application.
        Equal(StartNoticeCase.Failed, StartNotice.Decide(Reading(false, 1, 91)), "case B: a start that failed at its own step");
        Equal(StartNoticeCase.Failed, StartNotice.Decide(Reading(false, null, null)), "case B: nothing recorded at all");
        Equal(StartNoticeCase.Failed, StartNotice.Decide(Reading(false, 1, 33)), "case B: a start that stopped inside itself");
        // Healthy and unknown: nothing on the screen. A notice that appears on a working machine is a notice
        // people learn to close without reading.
        Equal(StartNoticeCase.None, StartNotice.Decide(Reading(true, 0, 61)), "healthy: no notice");
        Equal(StartNoticeCase.None, StartNotice.Decide(Reading(true, 2, 90)), "a running driver after a refused start of an earlier boot: no notice");
        Equal(StartNoticeCase.None, StartNotice.Decide(Reading(false, 2, 90, installed: false)), "no driver installed: no notice");
        Equal(StartNoticeCase.None, StartNotice.Decide(Reading(false, 2, 90, read: false)), "nothing could be read: no notice");
        Equal(StartNoticeCase.None, StartNotice.Decide(null), "no reading at all: no notice");
        Check(StartNotice.OffersEnable(StartNoticeCase.Guard) && !StartNotice.OffersEnable(StartNoticeCase.Failed) &&
            !StartNotice.OffersEnable(StartNoticeCase.None), "only case A offers the one-click repair");
        // The limit is the driver's own, not a number of this file.
        Equal(2u, Recovery.MaxUnconfirmedStarts, "the limit is BC250_MAX_UNCONFIRMED_STARTS");
        var kmd = File.ReadAllText(Path.Combine(root, @"driver\kmd\bc250kmd.h"));
        Check(kmd.Contains("#define BC250_MAX_UNCONFIRMED_STARTS " + Recovery.MaxUnconfirmedStarts), "the limit is the driver's own define");
        Check(kmd.Contains("StageRefusedByGuard = " + Recovery.StageGuardRefused), "the refusing stage is the driver's own StageRefusedByGuard");

        // Both cases have a title, a body of at least two lines and no missing text, in all four languages.
        var before = Strings.Language;
        foreach (var lang in Strings.Languages)
        {
            Strings.Language = lang;
            foreach (var c in new[] { StartNoticeCase.Guard, StartNoticeCase.Failed })
            {
                var title = StartNotice.Title(c);
                var text = StartNotice.Text(c);
                Check(!title.StartsWith("[", StringComparison.Ordinal) && title.Length > 5, "notice " + lang + " " + c + ": a title");
                Check(!text.StartsWith("[", StringComparison.Ordinal) && text.Split('\n').Length >= 2, "notice " + lang + " " + c + ": a body of more than one line");
                foreach (var x in PlainWords.Findings(new[] { title, text })) Check(false, "G-NOINT: notice " + lang + " " + c + ": " + x);
            }
            // The words of the window's buttons and of its results exist in every language.
            foreach (var id in new[] { "notice.open-app", "notice.close", "notice.working", "notice.working.seconds", "notice.done",
                "notice.action-failed", "notice.restart-needed", "notice.not-needed", "notice.open-failed", "status.action.enable-driver" })
                Check(!Strings.T(id).StartsWith("[", StringComparison.Ordinal), "notice " + lang + ": " + id);
        }
        Strings.Language = before;

        // ---- the sign-in task starts the notice and shows nothing else ---------------------------------------------
        // The blue start window is gone: the release's sign-in script starts the notice and
        // has no window, no switch and no preference of its own.
        var windowScript = Path.Combine(root, @"tools\release\installer\start-window.ps1");
        if (File.Exists(windowScript))
        {
            var text = File.ReadAllText(windowScript);
            Check(text.Contains("--start-notice"), "start-window.ps1 starts the sign-in notice");
            Check(!text.Contains("Windows.Forms") && !text.Contains("ShowStartWindow"), "start-window.ps1 shows no window and reads no switch");
        }
        else Check(false, "start-window.ps1 is in the repository: " + windowScript);
        Check(!SettingsSearch.Index.Any(e => e.Id == "settings.start-window"), "the settings have no switch for a start window");

        // ---- the plan of the one-click repair ------------------------------------------------------------------
        Check(Recovery.Actions.Contains("enable-driver"), "the action list has enable-driver");
        var refused = Guarded();
        refused.DriverError = "no answer";
        refused.Interop = null; refused.Health = null; refused.Dpm = null;
        Check(Recovery.GuardRefused(refused), "the support view and the notice agree: this reading is a refused start");
        var p = Recovery.Plan("enable-driver", refused);
        Check(!p.Refused && p.EnableDriver && !p.Undoable && p.Effect == "at once", "enable-driver: planned, at once, no undo");
        Equal(0, p.Writes.Count, "enable-driver writes no value of the general allow-list");
        Check(!Recovery.Allowed(Recovery.ParametersPath, GuardReset.ValueName), "the count stays outside the allow-list of the actions and of the undo");
        var dialog = PlainPlan.Describe(p);
        Check(!dialog.Title.StartsWith("[", StringComparison.Ordinal) && dialog.Changes.Count == 2, "enable-driver: the dialog says what it does");
        foreach (var t in new[] { dialog.Title }.Concat(dialog.Changes).Concat(dialog.Notes))
        {
            Check(!t.Contains("[plan.") && !t.Contains("[notice."), "enable-driver dialog text exists: " + t);
            foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: enable-driver dialog: " + x);
        }
        // Refused where it would change nothing that is wrong, with words for the person in every case.
        var running = Guarded();
        var onRefusal = Recovery.Plan("enable-driver", running);
        Check(onRefusal.Refused && onRefusal.Refusal.Contains("running already"), "a running driver: refused");
        Check(PlainPlan.Refusal(onRefusal) == Strings.T("plan.refuse.driver-on"), "a running driver: the refusal is in the window's words");
        var otherFailure = Guarded();
        otherFailure.DriverError = "no answer";
        otherFailure.Interop = null; otherFailure.Health = null; otherFailure.Dpm = null;
        otherFailure.Parameters["UnconfirmedStarts"] = 1;
        otherFailure.Parameters["LastStage"] = 91;
        var otherPlan = Recovery.Plan("enable-driver", otherFailure);
        Check(otherPlan.Refused && otherPlan.Refusal.StartsWith("No start was refused", StringComparison.Ordinal), "a start that failed for another reason: refused");
        Check(PlainPlan.Refusal(otherPlan) == Strings.T("plan.refuse.not-guard"), "a start that failed for another reason: the refusal is in the window's words");
        Check(Recovery.Plan("enable-driver", new RecoverySnapshot()).Refused, "no driver installed: refused");

        // The support view recommends the action now instead of the release installer, and the Home card carries
        // the same button, so the three places never send the person somewhere else.
        var line = Recovery.Describe(refused).First(l => l.Topic == "Driver start");
        Equal("enable-driver", line.Action, "the support view recommends the action");
        Check(!line.Text.Contains("run the release installer again"), "the support view no longer sends the person to the installer for this");
        var card = HomeStatus.Compute(new StatusInputs { Snapshot = refused });
        var item = card.Items.First();
        Equal("enable-driver", item.Action, "the Home card offers the same action");
        Equal(Strings.T("status.action.enable-driver"), item.ActionLabel, "the Home card uses the same button text as the notice");
        Check(item.Text == Strings.T("status.driver-refused") && item.Next == Strings.T("status.driver-refused.next"), "the Home card says what happened and what to do");
        foreach (var x in PlainWords.Findings(new[] { item.Text, item.Next, item.ActionLabel })) Check(false, "G-NOINT: the Home banner: " + x);
        var plainFailure = HomeStatus.Compute(new StatusInputs { Snapshot = otherFailure }).Items.First();
        Equal("restart", plainFailure.Action, "a start that failed for another reason keeps the restart advice on Home");

        // ---- the one registry write, under a stand-in ---------------------------------------------------------
        var api = new FakeRegApi();
        api.Values[GuardReset.ValueName] = 2;
        var r = GuardReset.Run(api);
        Check(r.Written && r.Before == 2u && r.After == 0u && r.Error == null, "the count 2 is written to 0 and reads back: " + r);
        Equal(0u, api.Values[GuardReset.ValueName], "the stand-in holds 0");
        Equal(1, api.Flushes, "the key is flushed, so the value survives a machine that stops at once");
        api = new FakeRegApi();
        r = GuardReset.Run(api);
        Check(r.Written && r.Before == null && r.After == 0u, "an absent count is written to 0: " + r);
        api = new FakeRegApi(); api.Values[GuardReset.ValueName] = 2; api.SetStatus = 5;
        r = GuardReset.Run(api);
        Check(!r.Written && r.Error != null && r.Error.Contains("5"), "a refused write is an error with its own number: " + r);
        Equal(2u, api.Values[GuardReset.ValueName], "a refused write leaves the count as it was");
        api = new FakeRegApi(); api.Values[GuardReset.ValueName] = 2; api.FlushStatus = 5;
        r = GuardReset.Run(api);
        Check(!r.Written && r.Error != null && r.Error.Contains("flush"), "a failed flush is never hidden: " + r);
        api = new FakeRegApi(); api.Values[GuardReset.ValueName] = 2; api.QueryStatus = 5;
        r = GuardReset.Run(api);
        Check(!r.Written && r.Error != null, "an unreadable count cannot be treated as a successful reset: " + r);
        Equal(2u, api.Values[GuardReset.ValueName], "an unreadable count is not overwritten");
        r = GuardReset.Run(null);
        Check(!r.Written && r.Error != null, "no registry access: an error, never a silent pass");

        // The helper and the window, by a scan of their sources (the elevated helper is not part of this test
        // program). The helper answers "the count is cleared, Windows has to restart" with a code of its own, and
        // the notice is the one place that turns that code into the restart button.
        var actionsSource = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\RecoveryActions.cs"));
        var noticeSource = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\NoticeForm.cs"));
        Check(actionsSource.Contains("public const int RestartNeeded = 6;"), "the helper has its own code for a change that needs a restart");
        Check(actionsSource.Contains("if (plan.EnableDriver) return EnableDriver();"), "the helper runs the enable-driver plan through its own step");
        Check(actionsSource.Contains("/restart-device") && !actionsSource.Contains("pnputil.Kill") && actionsSource.Contains("it is left running"),
            "the helper restarts the device with pnputil and never kills it");
        Check(actionsSource.Contains("GuardReset.Run(api)") && actionsSource.Contains("guard count again"),
            "the helper clears the count before the device and again when the new start stays unconfirmed");
        Check(actionsSource.Contains("Confirm(EnableConfirmSeconds)"), "the helper confirms the start it got with the existing confirmation");
        Check(noticeSource.Contains("RecoveryRunner.RestartNeeded ? \"notice.restart-needed\""), "the notice turns that code into the restart advice");
        Check(noticeSource.Contains("\"--action\", \"enable-driver\""), "the notice runs the same action as the Home banner");

        // ---- what each caller of the repair does with the helper's answer -------------------------------------
        // The helper is a process of its own, so these are scans of the sources that call it. A wrong mapping here
        // is the defect that tells a person the repair failed on the one path where it worked: the Home banner and
        // the support view reach the helper through MainForm.RunAction, and only the notice had code 6.
        var windowActions = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\MainForm.Actions.cs"));
        Check(windowActions.Contains("code == RecoveryRunner.RestartNeeded") && windowActions.Contains("Strings.T(\"notice.restart-needed\")"),
            "the window says that only a restart of Windows is left, instead of reporting a failure");
        Check(windowActions.Contains("Result(text, ok || restartNeeded)"), "a change that needs a restart is not drawn as a failure");
        Check(windowActions.Contains("if (restartNeeded || ok && plan.OfferRestart) OfferRestart()"), "the window offers that restart itself");
        // The state line of the support view recommends the action; the same card has to carry its button, which
        // is also what INSTALL.md sends a person to after they closed the notice.
        var helpSource = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\MainForm.Help.cs"));
        Check(Regex.IsMatch(helpSource, @"act\(""[^""]*"", ""enable-driver""\)"), "the support view has a button for the action its own state line recommends");
        // The notice: Close, and with it Escape, stays usable through the whole repair, and the repair's button
        // goes as soon as there is nothing left to repair, so a second click cannot turn a refusal into a false
        // alarm with a pointless restart offer.
        Check(noticeSource.Contains("c.Enabled = !working || c == _close"), "the notice keeps Close and Escape usable while the repair runs");
        Check(noticeSource.Contains("if (code == RecoveryRunner.Done) { Drop(_enable);"), "the repair's button goes after the repair worked");
        Check(noticeSource.Contains("if (code == RecoveryRunner.Refused) { Drop(_enable); Result(Strings.T(\"notice.not-needed\")"),
            "a refusal reads as 'not needed any more', not as a failure with a restart offer");
        // The window promises a bound and the helper has to keep it, end to end: the device query, the device
        // restart, the wait for the driver, then the confirmation.
        int budget = new[] { @"TimeSpan\.FromSeconds\((\d+)\)", @"DeviceRestartSeconds = (\d+)", @"DeviceBackSeconds = (\d+)", @"EnableConfirmSeconds = (\d+)" }
            .Sum(rx => int.Parse(Regex.Match(actionsSource, rx).Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture));
        Check(budget > 120 && budget <= 240, "the repair's own budget is " + budget + " s, inside the four minutes the window promises");
        Check(Strings.In("en", "notice.working").Contains("four minutes"), "the window names a bound the helper keeps");
        // Case A is on the screen at once; only the case that a driver still inside
        // its own start looks like waits out the poll. And the layout gate of the window covers the text scale of
        // Windows, which is the setting that makes a button's words outgrow its row.
        var programSource = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\Program.cs"));
        Check(Regex.IsMatch(programSource, @"if \(decided == StartNoticeCase\.Guard\) break;[\s\S]{0,200}ProbeSeconds"),
            "a refused start is shown without waiting out the probe");
        Check(Regex.IsMatch(programSource, @"foreach \(var textScale in new\[\] \{[^}]*2\.25f"), "the notice's layout gate covers the text scale of Windows");
        // The logon task starts the notice with no startup information: Windows overrides the first ShowWindow of
        // a process whose launcher passed STARTUPINFO, and a WinForms window shows itself exactly once.
        if (File.Exists(windowScript))
        {
            var startText = File.ReadAllText(windowScript);
            Check(!Regex.IsMatch(startText, @"Start-Process[^\r\n]*--start-notice[^\r\n]*-WindowStyle") &&
                !Regex.IsMatch(startText, @"-WindowStyle[^\r\n]*--start-notice"), "the notice is not started with a window style, so its window is shown");
        }
    }

    // A reading of a machine whose last start the driver's own guard refused: nothing of the driver answers, the
    // count is at the limit and the last stage is the refusal. Guarded() itself still has the driver answering,
    // so the plan's "already running" refusal has a reading too.
    static RecoverySnapshot Guarded()
    {
        var s = new RecoverySnapshot
        {
            DriverInstalled = true, RouterInstalled = true,
            Parameters = new Dictionary<string, long> { { "UnconfirmedStarts", 2 }, { "LastStage", 90 }, { "InteropLastState", 768 } },
            Interop = new InteropState { Flags = InteropState.FlagValid, Effective = InteropState.SwitchBlit | InteropState.SwitchCdd, Version = 0x000700D8 },
            Health = new StartHealthState { Flags = 7, Completed = 1, ReadyAgeMs = 70000, LastCompletionAgeMs = 100, Version = 0x000700D8 },
        };
        return s;
    }
}
