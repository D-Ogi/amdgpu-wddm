// Unit tests of the setup window's pure parts. Arguments: <repo root> <strings dir>. Exit 0 = all passed.
// G-STR (string tables), G-NOINT (tables), the engine contract (every check id, stage and message id the engine
// scripts emit has its words here), the event model and result binding (A2), the A3 texts, the settings-impact
// lines, release notes, prepared folders, the guide ranks, argument parsing and command-line quoting.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using AmdgpuWddmSetup;

static class UnitTests
{
    static int _failed, _passed;

    static void Check(bool ok, string what)
    {
        if (ok) _passed++;
        else { _failed++; Console.WriteLine("FAIL " + what); }
    }

    static void Equal<T>(T want, T got, string what) { Check(Equals(want, got), what + " (want " + want + ", got " + got + ")"); }

    static int Main(string[] args)
    {
        var repo = args[0];
        Strings.Directory = args[1];
        try
        {
            StringTables();
            EngineContract(repo);
            Model();
            ResultBinding();
            Views();
            RailSteps();
            Settings();
            Notes(repo);
            Prepared();
            Guide();
            Arguments();
            SmallScreens();
            NoConfirmations();
            SingleFile();
            LocaleContracts();
            Quoting();
            NativeLayout();
            Redaction();
        }
        catch (Exception e) { _failed++; Console.WriteLine("FAIL exception: " + e); }
        Console.WriteLine(_passed + " passed, " + _failed + " failed");
        return _failed == 0 ? 0 : 1;
    }

    static void StringTables()
    {
        var problems = Strings.Validate(Strings.Tables);
        foreach (var p in problems.Take(40)) Console.WriteLine("G-STR: " + p);
        Equal(0, problems.Count, "G-STR: setup string tables complete, fresh and consistent");
        foreach (var lang in Strings.Languages)
            foreach (var e in Strings.Tables[lang].Entries.Values)
            {
                var hits = NoInternals.Find(e.Text);
                Check(hits.Count == 0, "G-NOINT: " + lang + " " + e.Id + " has no internals (" + string.Join(",", hits) + ")");
            }
        Check(NoInternals.Find("the KMD fence 0x1F failed, see install.ps1").Count == 4, "G-NOINT catches KMD, fence, a hex code and a script name");
        Check(NoInternals.Find("amdgpu-wddm Control").Count == 0, "G-NOINT allows the product name");
        var queue = Strings.ReviewQueue(Strings.Tables);
        Console.WriteLine("G-STR review queue: " + queue.Count + " translations not reviewed");
    }

    // Every id the engine scripts can emit has its words in the window, and every message id is one ResultView knows.
    static void EngineContract(string repo)
    {
        var dir = Path.Combine(repo, "tools", "release", "installer");
        var install = File.ReadAllText(Path.Combine(dir, "install.ps1"));
        var prepare = File.ReadAllText(Path.Combine(dir, "prepare-offline.ps1"));
        var checkIds = new HashSet<string>();
        foreach (Match m in Regex.Matches(install, @"Add-Check\s+'[^']*'\s+'(ok|warn|fail)'[^\r\n]*?'([a-z0-9-]+\.[a-z0-9-]+)'\s*\)?\s*\}?\s*(?:else|$)", RegexOptions.Multiline)) checkIds.Add(m.Groups[2].Value);
        foreach (Match m in Regex.Matches(install, @"'(testsigning\.(active|inactive))'")) checkIds.Add(m.Groups[1].Value);
        foreach (Match m in Regex.Matches(prepare, @"Stop-Refused\s+'([a-z0-9-]+\.[a-z0-9-]+)'")) checkIds.Add(m.Groups[1].Value);
        Check(checkIds.Count >= 30, "engine contract: check ids found in the scripts (" + checkIds.Count + ")");
        foreach (var id in checkIds) Check(Strings.Has("check." + id), "engine contract: check." + id + " has words");
        foreach (Match m in Regex.Matches(install + prepare, @"Enter-Stage\s+'([a-z-]+)'")) Check(Strings.Has("stage." + m.Groups[1].Value), "engine contract: stage." + m.Groups[1].Value + " has words");
        var known = new HashSet<string>(ResultView.KnownMessageIds);
        var engine = File.ReadAllText(Path.Combine(dir, "engine.ps1"));
        foreach (Match m in Regex.Matches(install + prepare + engine, @"'(result\.[a-z-]+)'"))
            Check(known.Contains(m.Groups[1].Value), "engine contract: " + m.Groups[1].Value + " is a known message id");
        foreach (var id in known)
        {
            Check(Strings.Has(id + ".title"), id + ".title has words");
            Check(Strings.Has(id + ".body"), id + ".body has words");
        }
        foreach (Match m in Regex.Matches(install, @"Exit-Engine\s+-Code\s+(\d+)\s+-Outcome\s+'([a-z-]+)'"))
        {
            var outcome = m.Groups[2].Value;
            Check(new[] { "planned", "completed", "already", "restart-required", "verified", "refused", "needs-consent", "verify-failed", "failed", "cancelled", "prepared" }.Contains(outcome), "engine contract: outcome " + outcome + " is one the window handles");
        }
    }

    const string Id = "run-1";

    static string Ev(int seq, string type, string fields, string invocation = Id, string schema = EngineRun.EventSchema)
    {
        return "{\"schema\":\"" + schema + "\",\"invocation\":\"" + invocation + "\",\"seq\":" + seq + ",\"utc\":\"x\",\"type\":\"" + type + "\"" + (fields.Length > 0 ? "," + fields : "") + "}";
    }

    static void Model()
    {
        var r = new EngineRun(Id);
        r.Apply(Ev(1, "start", "\"mode\":\"run\",\"gui\":true,\"dry_run\":false,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":null"));
        r.Apply(Ev(2, "stage", "\"id\":\"preflight\",\"text\":\"x\""));
        r.Apply(Ev(3, "check", "\"id\":\"gpu.ok\",\"result\":\"ok\",\"name\":\"n\",\"detail\":\"d\""));
        r.Apply(Ev(4, "decision", "\"action\":\"upgrade\",\"installed_version\":\"0.7.198\",\"package_version\":\"0.7.199\",\"phase\":\"verified\",\"consents\":[\"test-signing\",\"bitlocker\"],\"restarts\":2,\"firmware_source\":\"package-folder\",\"notes\":[\"RELEASE-NOTES.md\"],\"compatibility\":{\"ok\":true,\"reasons\":[]}"));
        r.Apply(Ev(5, "cancel", "\"available\":true,\"where\":\"before-changes\""));
        Check(r.CancelAvailable, "model: cancel offered");
        r.Apply(Ev(6, "cancel", "\"available\":false,\"where\":\"driver-install\""));
        r.Apply(Ev(7, "step", "\"description\":\"x\",\"dry_run\":true"));
        Check(!r.MayHaveChanged, "model: a dry-run step is not a change");
        r.Apply(Ev(8, "install-action", "\"action\":\"upgrade\",\"boot_id\":4"));
        r.Apply(Ev(9, "step", "\"description\":\"x\",\"dry_run\":false"));
        r.Apply(Ev(9, "step", "\"description\":\"x\",\"dry_run\":false", "other-run"));
        r.Apply(Ev(10, "stage", "\"id\":\"files\",\"text\":\"x\"", Id, "amdgpu-wddm.engine-event/2"));
        r.Apply("{not json");
        r.Apply(Ev(10, "restart-required", "\"reason_id\":\"restart.driver-package\",\"continuation\":\"c\""));
        r.Apply(Ev(10, "result", "\"outcome\":\"restart-required\",\"exit_code\":0,\"message_id\":\"result.restart-driver\",\"mutated\":true"));
        Equal("run", r.Mode, "model: mode");
        Equal("upgrade", r.Decision.Action, "model: decision action");
        Equal(2, r.Decision.Consents.Length, "model: consents");
        Equal("package-folder", r.Decision.FirmwareSource, "model: firmware source");
        Check(r.Decision.CompatibilityOk, "model: compatibility");
        Check(!r.CancelAvailable, "model: cancel withdrawn");
        Check(r.MayHaveChanged && r.InstallActionSeen && r.RealSteps == 1, "model: the install action and the real step count");
        Equal(3, r.Ignored, "model: other run, other schema and broken line ignored");
        Check(r.Problems.Count == 1, "model: a repeated sequence number is recorded");
        Equal("restart.driver-package", r.RestartReason, "model: restart reason");
        Equal("preflight", string.Join(",", r.Stages), "model: stages of this run only");
        Equal("restart-required", r.ResultOutcome, "model: result event");
        Check(EngineRun.ExpectedStages("prepare-offline", null).SequenceEqual(new[] { "prepare-check", "firmware", "copy", "finish" }), "model: prepare stages");
    }

    static string Result(string invocation, string outcome, int exit, string message, bool mutated, string extra = "", string schema = EngineRun.ResultSchema)
    {
        return "{\"schema\":\"" + schema + "\",\"invocation\":\"" + invocation + "\",\"engine\":{\"contract\":\"c\",\"package_version\":\"v\"},\"mode\":\"run\",\"dry_run\":false,\"outcome\":\"" + outcome +
            "\",\"exit_code\":" + exit + ",\"mutated\":" + (mutated ? "true" : "false") + ",\"nothing_changed\":" + (mutated ? "false" : "true") + ",\"message_id\":\"" + message + "\"" + extra + "}";
    }

    static void ResultBinding()
    {
        string problem;
        Check(EngineResult.Read(Result(Id, "verified", 0, "result.verified", false), Id, out problem) != null, "A2: a result of this run is believed");
        Check(EngineResult.Read(Result("stale", "verified", 0, "result.verified", false), Id, out problem) == null && problem.Contains("another run"), "A2: a stale result of another run is not");
        Check(EngineResult.Read(Result(Id, "verified", 0, "result.verified", false, "", "amdgpu-wddm.engine-result/9"), Id, out problem) == null, "A2: an unknown result schema is not");
        Check(EngineResult.Read(null, Id, out problem) == null, "A2: no result file, no result");
        Check(EngineResult.Read("{\"schema\":\"" + EngineRun.ResultSchema + "\",\"invocation\":\"" + Id + "\"}", Id, out problem) == null, "A2: a result without an outcome is not");
        var r = EngineResult.Read(Result(Id, "restart-required", 0, "result.restart-test-signing", true, ",\"restart\":{\"required\":true,\"reason_id\":\"restart.test-signing\",\"continuation\":{\"command\":\"x\"}},\"consents_needed\":[],\"failed_checks\":[\"gpu.missing\"]"), Id, out problem);
        Check(r.RestartRequired && r.RestartReason == "restart.test-signing" && r.ContinuationCommand == "x" && r.FailedChecks.Length == 1, "A2: restart and continuation read");
        // The engine's automatic diagnostics of a driver start that did not pass: the window only carries the
        // path, and the support file adds that file as run-<n>-diagnostics.txt.
        var d = EngineResult.Read(Result(Id, "verify-failed", 3, "result.verify-failed", false, ",\"diagnostics\":\"C:\\\\ProgramData\\\\amdgpu-wddm\\\\installer\\\\diagnostics-x.txt\""), Id, out problem);
        Check(d.Diagnostics == @"C:\ProgramData\amdgpu-wddm\installer\diagnostics-x.txt", "A2: the diagnostics path of a failed start is read");
        Check(EngineResult.Read(Result(Id, "verified", 0, "result.verified", false), Id, out problem).Diagnostics == null, "A2: a run that collected nothing names no diagnostics file");
    }

    static ResultView V(string outcome, int exit, string message, bool mutated, string extra = "")
    {
        string problem;
        return ResultView.For(EngineResult.Read(Result(Id, outcome, exit, message, mutated, extra), Id, out problem), new EngineRun(Id));
    }

    static void Views()
    {
        var v = V("refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"gpu.missing\"]");
        Check(v.Kind == ViewKind.Problem && v.NothingChanged && !v.ChangesMade && v.FailedChecks.Length == 1 && v.OfferRetry, "A3: a pre-mutation refusal says nothing was changed and names the checks");
        v = V("failed", 6, "result.step-failed", true);
        Check(v.Kind == ViewKind.Problem && !v.NothingChanged && v.ChangesMade, "A3: a failure after changes never says nothing was changed");
        v = V("cancelled", 8, "result.cancelled-after-changes", true);
        Check(v.Kind == ViewKind.Cancelled && !v.NothingChanged && v.ChangesMade, "A3: cancelled after changes");
        v = V("cancelled", 8, "result.cancelled", false);
        Check(v.Kind == ViewKind.Cancelled && v.NothingChanged, "A3: cancelled before changes");
        var run = new EngineRun(Id);
        run.Apply(Ev(1, "install-action", "\"action\":\"install\",\"boot_id\":1"));
        v = ResultView.For(null, run);
        Check(v.Kind == ViewKind.Problem && v.ChangesUnknown && v.ChangesMade && !v.NothingChanged, "A3: no bound result: setup cannot tell, never 'nothing was changed'");
        v = V("restart-required", 0, "result.restart-driver", true);
        Check(v.Kind == ViewKind.Restart && v.OfferRestart && !v.NothingChanged, "C17: the window offers the restart");
        v = V("restart-required", 5, "result.testsigning-secureboot", false);
        Check(v.Kind == ViewKind.Problem && !v.OfferRestart, "Secure Boot on: no restart offered");
        v = V("restart-required", 7, "result.restart-still-pending", false);
        Check(v.Kind == ViewKind.Restart && v.OfferRestart, "same boot: restart offered again");
        v = V("needs-consent", 4, "result.needs-consent", false, ",\"consents_needed\":[\"bitlocker\"]");
        Check(v.Kind == ViewKind.Plan && v.ConsentsNeeded.Contains("bitlocker"), "needs-consent goes back to the plan");
        Check(V("verified", 0, "result.verified", false).Kind == ViewKind.Success, "verified is a success");
        Check(V("already", 0, "result.already", false).OfferRepair, "already offers a repair");
        Check(V("verify-failed", 3, "result.verify-failed", false).OfferRepair, "verify-failed offers a repair");
        Equal("result.other.title", V("refused", 2, "result.some-future-id", false).TitleId, "an unknown message id has a generic text");
        Check(V("prepared", 0, "result.offline-prepared", false).Kind == ViewKind.Success, "prepared is a success");

        // Try again is the default only when a new try can help (gui-L4 trials: a development installation).
        Check(!V("refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"lab-install.present\"]").RetryHelps, "retry: a development installation is not fixed by a new try");
        Check(!V("refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"gpu.missing\",\"secureboot.on\"]").RetryHelps, "retry: no BC-250 and Secure Boot on");
        Check(V("refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"gpu.missing\",\"space.low\"]").RetryHelps, "retry: one fixable check keeps Try again the default");
        Check(V("refused", 2, "result.firmware-unreachable", false, ",\"failed_checks\":[\"firmware.unreachable\"]").RetryHelps, "retry: an unreachable download can work on a new try");
        Check(V("refused", 2, "result.preflight-error", false).RetryHelps, "retry: an error without failed checks keeps Try again");
        Check(!V("refused", 2, "result.package-damaged", false).RetryHelps, "retry: damaged setup files need a new download");
        Check(ResultView.For(null, new EngineRun(Id)).RetryHelps, "retry: no bound result keeps Try again");
        foreach (var id in ResultView.RetryCannotFix) Check(Strings.Has("check." + id), "retry: check." + id + " is a check id the window knows");

        // The engine planned "already" but refused: the page says that this version is installed.
        v = V("refused", 2, "result.preflight-refused", false, ",\"action\":\"already\",\"failed_checks\":[\"lab-install.present\"]");
        Check(v.AlreadyInstalled && v.PackageVersion == "v", "already: a refusal of an installed version says so");
        Check(!V("already", 0, "result.already", false, ",\"action\":\"already\"").AlreadyInstalled, "already: the already result has its own title");
        Check(!V("refused", 2, "result.preflight-refused", false, ",\"action\":\"install\"").AlreadyInstalled, "already: not for an install");
        Check(Strings.Has("result.already-installed"), "already: result.already-installed has words");
    }

    static string Rails(RailState[] s) { return string.Join(",", s.Select(x => x.ToString())); }

    static void RailSteps()
    {
        var refused = V("refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"lab-install.present\"]");
        Equal("Done,Failed,Pending,Pending,Pending", Rails(Rail.States(5, 4, refused, "plan")), "rail: a refusal at Check (gui-L4 trials)");
        Equal("Done,Done,Done,Failed,Pending", Rails(Rail.States(5, 4, V("failed", 6, "result.step-failed", true), "install")), "rail: a failure during Install");
        Equal("Done,Done,Done,Current,Pending", Rails(Rail.States(5, 4, V("cancelled", 8, "result.cancelled", false), "install")), "rail: a cancel during Install is current, not failed");
        Equal("Done,Done,Done,Done,Current", Rails(Rail.States(5, 4, V("verified", 0, "result.verified", false), "install")), "rail: success at Finish");
        Equal("Done,Failed,Pending", Rails(Rail.States(3, 2, V("refused", 2, "result.prepare-destination", false), "prepare")), "rail: a prepare refusal");
        Equal("Done,Done,Current,Pending,Pending", Rails(Rail.States(5, 2, null, "plan")), "rail: the Review screen");
        Equal("Done,Failed,Pending,Pending,Pending", Rails(Rail.States(5, 4, ResultView.For(null, new EngineRun(Id)), "plan")), "rail: a plan without a result");
        foreach (var id in new[] { "rail.done", "rail.failed", "rail.current", "check.mark.ok", "check.mark.warn", "check.mark.fail", "guide.checking" }) Check(Strings.Has(id), id + " has words");
        // The words of the window frame and of the screens the new layout added: the title bar glyphs, the three
        // promises of the welcome screen, the count of checks that need attention and the elapsed time of a run.
        foreach (var id in new[] { "ui.minimise", "ui.close-window", "welcome.promise.driver", "welcome.promise.shown", "welcome.promise.offline", "plan.checks.attention", "working.elapsed" })
            Check(Strings.Has(id), id + " has words");
        Check(Strings.In("en", "plan.checks.attention", 2).Contains("2"), "the count of checks that need attention reaches the sentence");
        Check(Strings.In("en", "working.elapsed", "1:05").Contains("1:05"), "the elapsed time reaches the sentence");
    }

    static void Settings()
    {
        var plan = new SettingsPlan();
        plan.Rows.Add(new SettingRow { Group = "parameters", Name = "DpmMaxMHz", Decision = "kept", Current = 1700, Value = 1500 });
        plan.Rows.Add(new SettingRow { Group = "parameters", Name = "CuMode", Decision = "kept", Current = 40 });
        plan.Rows.Add(new SettingRow { Group = "parameters", Name = "DpmMode", Decision = "set", Value = 0 });
        plan.Rows.Add(new SettingRow { Group = "desktop_router", Name = "DwmForceCpu", Decision = "same", Current = 1, Value = 1 });
        plan.Rows.Add(new SettingRow { Group = "d3d12:witcher3.exe", Name = "Experiment", Decision = "update", Value = "a,b" });
        plan.Rows.Add(new SettingRow { Group = "parameters", Name = "EnableMmio", Decision = "same", Value = 1 });
        plan.Rows.Add(new SettingRow { Group = "parameters", Name = "EnableFullWddm", Decision = "update", Value = 2 });
        plan.Rows.Add(new SettingRow { Group = "app_router", Name = "Mode", Decision = "update", Current = "allowlist", Value = "gpu-default" });
        plan.Rows.Add(new SettingRow { Group = "app_router", Name = "Allow", Decision = "same", Value = "dxdiag.exe" });
        var lines = SettingsView.Lines(plan);
        Equal(7, lines.Count, "settings: six named lines and one for the rest");
        var text = string.Join("\n", lines.Select(l => Strings.In("en", l.TextId, l.Args) + (l.DecisionId != null ? ": " + Strings.In("en", l.DecisionId) : "")));
        Check(text.Contains("Graphics clock limit: 1700 MHz: your value is kept"), "settings: a kept value shows the user's value");
        Check(text.Contains("Automatic graphics clock: off") && text.Contains("Graphics cores: 40") && text.Contains("processor (safe mode)") && text.Contains("witcher3.exe"), "settings: the named settings in plain words");
        Check(text.Contains("3 other driver settings, 1 of them new or changed"), "settings: the rest counted");
        Check(text.Contains("Direct3D 11 and 10.1 games and apps drawn by the graphics chip") && !text.Contains("gpu-default"), "settings: the D3D11 route of tester.11 in plain words (GPU, Windows' own apps on the processor)");
        var old = new SettingsPlan();
        old.Rows.Add(new SettingRow { Group = "app_router", Name = "Mode", Decision = "kept", Current = "allowlist", Value = "gpu-default" });
        Check(SettingsView.Lines(old)[0].TextId == "settings.d3d11.listed", "settings: a kept allowlist route says so");
        Check(!Regex.IsMatch(text, "Dpm|Enable|Dwm|Experiment|parameters"), "settings: no registry names in the window");
        foreach (var d in new[] { "set", "same", "update", "kept", "command", "reopened", "driver-closed" }) Check(Strings.Has("settings.decision." + d), "settings: decision " + d + " has words");
        // BD-069: a switch the driver closed itself is the driver's act, not the user's choice, and this install
        // changes nothing by keeping it. A repair sets it back. The closed desktop path gets a line of its own,
        // because the user has to read there why the desktop stays on the processor and what sets it back.
        var closed = new SettingsPlan();
        closed.Rows.Add(new SettingRow { Group = "parameters", Name = "DpmMode", Decision = "driver-closed", Current = 0, Value = 0, Default = 1 });
        closed.Rows.Add(new SettingRow { Group = "parameters", Name = "EnableGpuPresentBlit", Decision = "driver-closed", Current = 0, Value = 0, Default = 1 });
        closed.Rows.Add(new SettingRow { Group = "parameters", Name = "EnableCddDwmInterop", Decision = "driver-closed", Current = 0, Value = 0, Default = 1 });
        closed.Rows.Add(new SettingRow { Group = "desktop_router", Name = "DwmForceCpu", Decision = "same", Current = 0, Value = 0 });
        var closedLines = SettingsView.Lines(closed);
        var closedText = string.Join("\n", closedLines.Select(l => Strings.In("en", l.TextId, l.Args) + (l.DecisionId != null ? ": " + Strings.In("en", l.DecisionId) : "")));
        Check(closedText.Contains("Automatic graphics clock: off: switched off by the driver. A repair sets it back"), "settings: a driver closure is reported as the driver's own act");
        Check(closedText.Contains("Desktop on the graphics chip: switched off by the driver. A repair sets it back"), "settings: a closed desktop path is named, with the repair as its remedy");
        Check(closedText.Contains("Desktop drawn by the processor until the driver draws it again") && !closedText.Contains("Desktop drawn by the graphics chip"), "settings: a closed desktop path does not promise the graphics chip");
        Equal(3, closedLines.Count, "settings: the two switches of the desktop path make one line, and none is left for the rest");
        Check(!Regex.IsMatch(closedText, "Dpm|Enable|Dwm|Interop|parameters"), "settings: no registry names in a closure line");
        var reopened = new SettingsPlan();
        reopened.Rows.Add(new SettingRow { Group = "parameters", Name = "DpmMode", Decision = "reopened", Current = 0, Value = 1, Default = 1 });
        reopened.Rows.Add(new SettingRow { Group = "parameters", Name = "EnableCddDwmInterop", Decision = "reopened", Current = 0, Value = 1, Default = 1 });
        reopened.Rows.Add(new SettingRow { Group = "desktop_router", Name = "DwmForceCpu", Decision = "same", Current = 0, Value = 0 });
        var reopenedText = string.Join("\n", SettingsView.Lines(reopened).Select(l => Strings.In("en", l.TextId, l.Args) + (l.DecisionId != null ? ": " + Strings.In("en", l.DecisionId) : "")));
        Check(reopenedText.Contains("Automatic graphics clock: on: set back by this repair"), "settings: a repair reports the value it sets back");
        Check(reopenedText.Contains("Desktop on the graphics chip: set back by this repair") && reopenedText.Contains("Desktop drawn by the graphics chip"), "settings: a reopened desktop path says so, and the desktop line promises the graphics chip again");
        // A switch of the desktop path that nobody closed stays among the other settings, as before.
        var open = new SettingsPlan();
        open.Rows.Add(new SettingRow { Group = "parameters", Name = "EnableGpuPresentBlit", Decision = "same", Current = 1, Value = 1 });
        var openLines = SettingsView.Lines(open);
        Equal(1, openLines.Count, "settings: an open desktop path adds no line of its own");
        Check(openLines[0].TextId == "settings.other", "settings: an untouched switch of the desktop path is counted among the rest");
    }

    static void Notes(string repo)
    {
        var md = "# amdgpu-wddm 1\n\nIntro text.\n\n## New\n\n- One `thing`\n  continued here.\n- **Two**\n\n## Fixed\n\n## Known issues\n\n## Settings affected\n";
        var plain = ReleaseNotes.Plain(md);
        Check(plain.Contains("\u2022 One thing continued here.") && plain.Contains("\u2022 Two") && !plain.Contains("#") && !plain.Contains("`"), "notes: plain text with bullets");
        Equal(0, ReleaseNotes.MissingSections(md).Count, "notes: the four sections found");
        Equal("Known issues", ReleaseNotes.MissingSections("## New\n## Fixed\n## Settings affected").Single(), "notes: a missing section named");
        var real = Directory.GetFiles(Path.Combine(repo, "docs", "testing", "release-notes"), "*.md");
        Check(real.Length > 0, "notes: release notes in the repository");
        foreach (var f in real) Equal(0, ReleaseNotes.MissingSections(File.ReadAllText(f)).Count, "notes: " + Path.GetFileName(f) + " has the four sections");
        // The window draws the notes as blocks, so the page shows headings, bullets and paragraphs, not a raw file.
        var blocks = NotesDoc.Parse(md);
        Equal(NoteKind.Title, blocks[0].Kind, "notes: the first heading is the title of the notes");
        Equal("amdgpu-wddm 1", blocks[0].Text, "notes: the title text");
        Equal(NoteKind.Paragraph, blocks[1].Kind, "notes: the text under the title is a paragraph");
        Equal(NoteKind.Heading, blocks[2].Kind, "notes: a section heading");
        var bullets = blocks.Where(b => b.Kind == NoteKind.Bullet).ToList();
        Equal(2, bullets.Count, "notes: the bullets of the section");
        Equal("One thing continued here.", bullets[0].Text, "notes: a bullet joins its wrapped line");
        Check(bullets.All(b => !b.Text.Contains("`") && !b.Text.Contains("*")), "notes: code marks and emphasis are gone");
        Check(blocks.All(b => !b.Text.StartsWith("#", StringComparison.Ordinal)), "notes: no heading marks in the text");
        Equal(0, NotesDoc.Parse(null).Count, "notes: no notes, no blocks");

        var dir = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-setup-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            File.WriteAllText(Path.Combine(dir, "RELEASE-NOTES.md"), md);
            var n = ReleaseNotes.Load(dir, "ja");
            Check(n.EnglishFallback && n.Language == "en", "C16: no Japanese notes: English, labelled");
            Check(n.Source == md && NotesDoc.Parse(n.Source).Count == blocks.Count, "notes: the window gets the notes as they are written, for its own blocks");
            File.WriteAllText(Path.Combine(dir, "RELEASE-NOTES.ja.md"), "## New\n- x\n");
            n = ReleaseNotes.Load(dir, "ja");
            Check(!n.EnglishFallback && n.Language == "ja", "C16: a localized notes file is used");
            Check(!ReleaseNotes.Load(dir, "en").EnglishFallback, "C16: English is not labelled in English");
        }
        finally { Directory.Delete(dir, true); }
    }

    static void Prepared()
    {
        var dir = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-setup-test-" + Guid.NewGuid().ToString("N"));
        string version;
        Equal("folder.missing", PreparedFolder.Check(dir, out version), "prepared: a missing folder");
        Directory.CreateDirectory(Path.Combine(dir, "installer"));
        try
        {
            Equal("folder.not-prepared", PreparedFolder.Check(dir, out version), "prepared: a folder without offline-set.json");
            File.WriteAllText(Path.Combine(dir, "offline-set.json"), "{\"schema\":\"amdgpu-wddm.offline-set/9\"}");
            Equal("folder.unknown", PreparedFolder.Check(dir, out version), "prepared: an unknown schema");
            File.WriteAllText(Path.Combine(dir, "offline-set.json"), "{\"schema\":\"" + PreparedFolder.Schema + "\",\"version\":\"0.7.199\"}");
            Equal("folder.incomplete", PreparedFolder.Check(dir, out version), "prepared: without its installer");
            File.WriteAllText(Path.Combine(dir, "manifest.json"), "{}");
            File.WriteAllText(Path.Combine(dir, "installer", "install.ps1"), "");
            Check(PreparedFolder.Check(dir, out version) == null && version == "0.7.199", "prepared: a complete folder and its version");
            foreach (var id in new[] { "folder.missing", "folder.not-prepared", "folder.unknown", "folder.incomplete" }) Check(Strings.Has(id), id + " has words");
        }
        finally { Directory.Delete(dir, true); }
    }

    static void Guide()
    {
        Equal(SetupCause.InstallStopped, SetupGuide.Pick(new[] { SetupCause.Welcome, SetupCause.PendingRestart, SetupCause.InstallStopped }), "guide: a problem wins");
        Equal(SetupCause.PendingRestart, SetupGuide.Pick(new[] { SetupCause.UpgradeDone, SetupCause.WorkInProgress, SetupCause.PendingRestart }), "guide: a pending restart wins over work and done");
        Equal(SetupCause.InstallStopped, SetupGuide.Pick(new[] { SetupCause.VerificationFailed, SetupCause.InstallStopped }), "guide: inside rank 1 the plan's order");
        Equal("07-hopeful", SetupGuide.Expression(SetupCause.PendingRestart), "guide: expression of a pending restart");
        foreach (SetupCause c in Enum.GetValues(typeof(SetupCause))) Check(Strings.Has(SetupGuide.TextId(c)), "guide: " + c + " has words");
        Check(Strings.In("en", "checking.body").Contains("up to a minute") && Strings.In("en", "guide.checking").Contains("up to a minute"), "guide: the Checking page and its tip give the same time");
        // The art the build embeds: every expression a cause can ask for, and nothing else. build.ps1 reads the line
        // below and embeds exactly these, so an expression that a new cause introduces is embedded by that alone.
        foreach (SetupCause c in Enum.GetValues(typeof(SetupCause)))
            Check(SetupGuide.ArtExpressions.Contains(SetupGuide.Expression(c)), "guide: the art of " + c + " (" + SetupGuide.Expression(c) + ") is embedded");
        foreach (var e in SetupGuide.ArtExpressions)
            Check(Enum.GetValues(typeof(SetupCause)).Cast<SetupCause>().Any(c => SetupGuide.Expression(c) == e), "guide: no expression is embedded that no cause asks for (" + e + ")");
        Console.WriteLine("guide art expressions: " + string.Join(" ", SetupGuide.ArtExpressions));
        // The picture never grows past 1.25 times the @256 art, at any screen scale, and the welcome panel gives her
        // more room than the other screens.
        foreach (var scale in new[] { 1f, 1.25f, 1.5f, 2f })
        {
            Check(SetupGuide.ArtBox(scale, true) <= SetupGuide.MaxArtPixels && SetupGuide.ArtBox(scale, false) <= SetupGuide.MaxArtPixels, "guide: the art stays within 1.25 x @256 at scale " + scale);
            Check(SetupGuide.ArtBox(scale, true) >= SetupGuide.ArtBox(scale, false), "guide: the welcome panel is not smaller at scale " + scale);
            Equal(256, SetupGuide.ArtSize(scale), "guide: the @256 art fills the slot at scale " + scale);
            Check(SetupGuide.ArtBox(scale, false) > 128, "guide: the smallest slot is larger than the @128 picture at scale " + scale + ", so only the @256 art is embedded");
        }
        Equal(SetupGuide.MaxArtPixels, SetupGuide.ArtBox(4f, true), "guide: a very large screen still draws the art at 1.25 x @256");
    }

    static void Arguments()
    {
        Check(!SetupArgs.Parse(new string[0]).ApplySystemTuning, "tuning: absent is not consent");
        Check(SetupArgs.Parse(new[] { "--apply-system-tuning" }).ApplySystemTuning, "tuning: explicit opt-in");
        Equal(0, SetupArgs.TuningArguments(null).Length, "tuning: continuation retains saved choice");
        Equal("-ApplySystemTuning", SetupArgs.TuningArguments(true)[0], "tuning: selected argument");
        Equal("-SkipSystemTuning", SetupArgs.TuningArguments(false)[0], "tuning: explicit deselection overrides resume");
        Equal("welcome", SetupArgs.Parse(new string[0]).Mode, "args: none = welcome");
        Equal("continue", SetupArgs.Parse(new[] { "--continue" }).Mode, "args: --continue");
        var a = SetupArgs.Parse(new[] { "--repair", "--package", @"C:\x y" });
        Check(a.Mode == "repair" && a.Package == @"C:\x y" && a.Error == null, "args: --repair --package");
        Check(SetupArgs.Parse(new[] { "--repair", "--continue" }).Error != null, "args: two modes refused");
        Check(SetupArgs.Parse(new[] { "--bogus" }).Error != null, "args: unknown refused");
        Check(SetupArgs.Parse(new[] { "--continue", "--dry-run" }).Error != null, "args: --dry-run only for welcome or repair");
        a = SetupArgs.Parse(new[] { "--smoke-render", "d", "1.5", "ja", "--text-scale", "1.5" });
        Check(a.Mode == "smoke-render" && a.Scale == 1.5f && a.Language == "ja" && a.TextScale == 1.5f && a.Error == null, "args: --smoke-render");
        Check(SetupArgs.Parse(new[] { "--smoke-render", "d", "1", "xx" }).Error != null, "args: unknown language refused");
        a = SetupArgs.Parse(new[] { "--smoke-engine", "p", "o", "--cancel", "--", "-DryRun", "-FirmwareDir", "f" });
        Check(a.Mode == "smoke-engine" && a.Cancel && a.EngineArgs.SequenceEqual(new[] { "-DryRun", "-FirmwareDir", "f" }), "args: --smoke-engine with engine arguments");
        Check(SetupArgs.Parse(new[] { "--", "-Force" }).Error != null, "args: engine arguments refused for the window");
        a = SetupArgs.Parse(new[] { "--smoke-render", "d", "1", "en", "--work-area", "800x600" });
        Check(a.WorkWidth == 800 && a.WorkHeight == 600 && a.Error == null, "args: --work-area 800x600");
        foreach (var bad in new[] { "800", "800x", "x600", "0x600", "800x0", "100x100", "abcxdef", "99999x600" })
            Check(SetupArgs.Parse(new[] { "--smoke-render", "d", "1", "en", "--work-area", bad }).Error != null, "args: --work-area refuses " + bad);
        Check(SetupArgs.Parse(new[] { "--repair", "--work-area", "800x600" }).Error != null, "args: --work-area only with --smoke-render");
    }

    // The owner's rule of 2026-10-12: on a 1024x768 or 800x600 working area, and on every smaller one, the guide
    // character and the whole right-hand panel she sits in do not show (GitHub #38: on the tester's 800x600 screen the
    // panel covered the text of the screen). The screen comes in through the rule's arguments, so these cases never
    // read the screen this test runs on; the window's own side of it is a render finding (--smoke-render G-SMALL, at
    // the same five working areas).
    static void SmallScreens()
    {
        foreach (var c in new[] {
                new { W = 800, H = 600, Small = true },
                new { W = 1024, H = 768, Small = true },
                new { W = 1280, H = 720, Small = true },       // 720 high: under the 768 of the rule
                new { W = 1366, H = 768, Small = true },       // exactly 768 high
                new { W = 1920, H = 1080, Small = false },
                new { W = 640, H = 480, Small = true },
                new { W = 1024, H = 1200, Small = true },      // exactly 1024 wide
                new { W = 1025, H = 769, Small = false } })    // one pixel over the rule in both directions
            Check(SmallScreen.Is(c.W, c.H) == c.Small, "small screen: " + c.W + "x" + c.H + " is " + (c.Small ? "small" : "not small"));
        Check(!SmallScreen.Is(0, 0) && !SmallScreen.Is(-1, 600) && !SmallScreen.Is(800, 0),
            "small screen: a working area that is not known is not small, so the panel stays where it was");
        // Logical units, not device pixels: the same monitor is a small screen at a large text size.
        Check(!SmallScreen.IsDevice(1920, 1032, 1f), "small screen: 1920x1032 at 100 % is not small");
        Check(SmallScreen.IsDevice(1920, 1032, 1.5f), "small screen: 1920x1032 at 150 % is 1280x688 logical, which is small");
        Check(SmallScreen.IsDevice(1600, 1200, 2f), "small screen: 1600x1200 at 200 % is 800x600 logical, which is small");
        Check(SmallScreen.IsDevice(2048, 1536, 2f), "small screen: 2048x1536 at 200 % is 1024x768 logical, which is small");
        Check(!SmallScreen.IsDevice(2100, 1600, 2f), "small screen: 2100x1600 at 200 % is 1050x800 logical, which is not small");
        Check(!SmallScreen.IsDevice(1920, 1080, 0f), "small screen: a scale that is not known decides nothing");
        Equal(1024, SmallScreen.MaxWidth, "small screen: the width of the rule the owner gave");
        Equal(768, SmallScreen.MaxHeight, "small screen: the height of the rule the owner gave");
    }

    // The owner's instruction of 2026-10-12: the installer asks the user for nothing. No test-mode check box, no
    // BitLocker option buttons, no Install button that waits for an answer, and no "type YES" on the command line.
    // Pressing Install is the consent. These cases hold the rules of InstallFlow.cs, the words of the four tables,
    // and the absence of the words the removed controls used. The window's own side of it is a render finding
    // (--smoke-render G-CONSENT), and the engine's side is tools/release/test-engine-units.ps1.
    static void NoConfirmations()
    {
        // Every run carries the switches that used to carry the answers, so an older engine asks nothing either.
        Check(InstallFlow.ConsentArguments(null).SequenceEqual(new[] { "-AcceptTestSigning" }),
            "no confirmations: a PC without BitLocker protection installs with the test-signing switch alone");
        foreach (var state in new[] { "off", "suspended", "unknown", "", null })
            Check(!InstallFlow.ConsentArguments(state).Contains("-BitLocker"),
                "no confirmations: BitLocker state '" + (state ?? "null") + "' is not suspended");
        Check(InstallFlow.ConsentArguments("on").SequenceEqual(new[] { "-AcceptTestSigning", "-BitLocker", "Suspend" }),
            "no confirmations: protection that is on is suspended for the restarts, without being asked about");
        Check(InstallFlow.SuspendBitLocker("on") && !InstallFlow.SuspendBitLocker("unknown") && !InstallFlow.SuspendBitLocker("off"),
            "no confirmations: only a state that was read and says on is suspendable");
        // What the plan screen states instead of asking.
        Check(InstallFlow.PlanChangeIds(true, "off").SequenceEqual(new[] { "plan.change.test-mode" }),
            "no confirmations: the plan states the test mode change");
        Check(InstallFlow.PlanChangeIds(true, "on").SequenceEqual(new[] { "plan.change.test-mode", "plan.change.bitlocker" }),
            "no confirmations: the plan states the BitLocker pause as information");
        Check(InstallFlow.PlanChangeIds(true, "unknown").SequenceEqual(new[] { "plan.change.test-mode", "plan.change.bitlocker.unknown" }),
            "no confirmations: a state that could not be read is one information line, not a question");
        Check(InstallFlow.PlanChangeIds(false, "on").SequenceEqual(new[] { "plan.change.test-mode.on" }),
            "no confirmations: test mode already on says so and nothing about the start options");
        foreach (var on in new[] { true, false })
            foreach (var state in new[] { "off", "on", "unknown", "suspended", null })
                foreach (var id in InstallFlow.PlanChangeIds(on, state))
                    Check(Strings.Has(id), "no confirmations: " + id + " has words");
        // The words of the removed controls are gone from every table, so neither can come back by itself.
        foreach (var id in InstallFlow.RemovedConsentIds)
            Check(!Strings.Has(id), "no confirmations: " + id + " is gone from the string tables");
        // The countdown of the restart screen: 15 s, one sentence with the seconds in it, in all four languages.
        Equal(15, InstallFlow.RestartCountdownSeconds, "no confirmations: the countdown the owner asked for");
        foreach (var lang in Strings.Languages)
        {
            Equal("0", Strings.Placeholders(Strings.In(lang, "restart.countdown")), "no confirmations: " + lang + " restart.countdown names the seconds");
            Check(Strings.In(lang, "restart.countdown.over").Trim().Length > 0, "no confirmations: " + lang + " says when the countdown is over");
        }
        // The restart screen keeps both offers, and neither is required: the countdown ends in a restart by itself.
        foreach (var id in new[] { "restart.now", "restart.later", "restart.countdown", "restart.countdown.over" })
            Check(Strings.Has(id), "no confirmations: the restart screen keeps " + id);
    }

    // The single-file installer (owner, 2026-10-12: "dlaczego setup.exe nie zawiera zipa w sobie?"). The reader's
    // own end-to-end run is --smoke-payload, driven by tools/release/test-setup-exe.ps1 with a packed exe and with
    // a damaged one; these cases hold the parts a test can hold without a package: the file list, the paths it is
    // allowed to name, and the permission list of the folder the package is unpacked into.
    static void SingleFile()
    {
        Equal(120, Payload.TrailerSize, "single file: the trailer is 120 bytes");
        Equal(16, Payload.Magic.Length, "single file: the magic is 16 bytes");
        Equal("files", Payload.FilesDir, "single file: the package sits in files\\ beside setup.exe");
        // A file with no trailer, and a file too short to hold one: no payload, no exception.
        var tmp = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-setup-tests");
        Directory.CreateDirectory(tmp);
        var plain = Path.Combine(tmp, "plain.bin");
        File.WriteAllBytes(plain, new byte[500]);
        Check(Payload.ReadHead(plain) == null, "single file: a file without the trailer carries no package");
        var tiny = Path.Combine(tmp, "tiny.bin");
        File.WriteAllBytes(tiny, new byte[8]);
        Check(Payload.ReadHead(tiny) == null, "single file: a file shorter than a trailer carries no package");
        Check(Payload.ReadHead(Path.Combine(tmp, "absent.bin")) == null, "single file: a file that is not there carries no package");
        // The file list.
        string version;
        var list = Payload.ParseList("version\t1.2.3\nfile\t" + new string('A', 64) + "\t7\tsetup.exe\n"
            + "file\t" + new string('B', 64) + "\t9\tfiles/installer/install.ps1\n", out version);
        Equal("1.2.3", version, "single file: the list names the release");
        Equal(2, list.Count, "single file: the list holds both files");
        Equal(@"files\installer\install.ps1", list[1].Path, "single file: a path of the list uses the separator of this system");
        foreach (var bad in new[] {
                "file\t" + new string('A', 64) + "\t7\tsetup.exe\n",                      // no version
                "version\t1.2.3\n",                                                        // no file
                "version\t1.2.3\nfile\t" + new string('A', 63) + "\t7\tsetup.exe\n",     // hash too short
                "version\t1.2.3\nfile\t" + new string('A', 64) + "\tx\tsetup.exe\n",     // size not a number
                "version\t1.2.3\nfile\t" + new string('A', 64) + "\t-1\tsetup.exe\n",    // negative size
                "version\t1.2.3\nfile\t" + new string('A', 64) + "\t7\t..\\evil.exe\n",  // outside the folder
                "version\t1.2.3\nsomething\telse\n" })                                    // a line this version does not know
        {
            string v;
            bool refused = false;
            try { Payload.ParseList(bad, out v); } catch (InvalidDataException) { refused = true; }
            Check(refused, "single file: the list is refused: " + bad.Replace("\n", " / ").Replace("\t", " "));
        }
        // The paths a payload may name. A zip entry's name decides where a file is written, so this is the rule
        // that keeps a doctored package inside its own folder.
        foreach (var ok in new[] { "setup.exe", @"files\manifest.json", @"files\installer\install.ps1" })
            Check(Payload.SafeRelative(ok), "single file: " + ok + " is a path inside the folder");
        foreach (var bad in new[] { "", @"\setup.exe", @"C:\setup.exe", @"..\setup.exe", @"files\..\..\x.exe",
                @"files\\x.exe", @"files\.\x.exe", "x:y" })
            Check(!Payload.SafeRelative(bad), "single file: " + (bad.Length == 0 ? "(empty)" : bad) + " is refused");
        // The permission list of the unpacked folder: administrators and SYSTEM may do anything, Users may read and
        // run and nothing more, nobody else is on it, and it is not inherited from %ProgramData% (where a standard
        // user may create folders). The files run elevated after the restarts of the install.
        var rules = Payload.Access.Rules();
        Check(rules.AreAccessRulesProtected, "single file: the folder does not inherit permissions");
        var aces = rules.GetAccessRules(true, false, typeof(System.Security.Principal.SecurityIdentifier))
            .Cast<System.Security.AccessControl.FileSystemAccessRule>().ToList();
        Equal(3, aces.Count, "single file: three entries on the folder");
        foreach (var ace in aces)
        {
            var sid = (System.Security.Principal.SecurityIdentifier)ace.IdentityReference;
            Check(ace.AccessControlType == System.Security.AccessControl.AccessControlType.Allow, "single file: no deny entry is needed");
            Check(ace.InheritanceFlags == (System.Security.AccessControl.InheritanceFlags.ObjectInherit | System.Security.AccessControl.InheritanceFlags.ContainerInherit),
                "single file: the entry reaches the files inside");
            bool admins = sid.IsWellKnown(System.Security.Principal.WellKnownSidType.BuiltinAdministratorsSid);
            bool system = sid.IsWellKnown(System.Security.Principal.WellKnownSidType.LocalSystemSid);
            bool users = sid.IsWellKnown(System.Security.Principal.WellKnownSidType.BuiltinUsersSid);
            Check(admins || system || users, "single file: only administrators, SYSTEM and Users are on the list");
            if (admins || system) Check(ace.FileSystemRights.HasFlag(System.Security.AccessControl.FileSystemRights.FullControl), "single file: full control for " + (admins ? "administrators" : "SYSTEM"));
            if (users)
            {
                // Windows adds Synchronize to every allow entry of its own; what matters is that nothing else is in it.
                var granted = ace.FileSystemRights & ~System.Security.AccessControl.FileSystemRights.Synchronize;
                Check(granted == System.Security.AccessControl.FileSystemRights.ReadAndExecute, "single file: Users may read and run, no more (" + granted + ")");
                foreach (var right in new[] { System.Security.AccessControl.FileSystemRights.WriteData,
                        System.Security.AccessControl.FileSystemRights.AppendData,
                        System.Security.AccessControl.FileSystemRights.CreateFiles,
                        System.Security.AccessControl.FileSystemRights.Delete,
                        System.Security.AccessControl.FileSystemRights.WriteAttributes })
                    Check(!ace.FileSystemRights.HasFlag(right), "single file: Users may not " + right);
            }
        }
        // Cleanup removes the version folder of the setup root and refuses anything else, whatever it is asked.
        foreach (var outside in new[] { null, "", @"C:\Windows\System32", Path.Combine(Path.GetTempPath(), "x"), Payload.Root })
            Payload.Cleanup(outside);
        Check(Directory.Exists(Path.GetTempPath()), "single file: Cleanup leaves a folder outside its own root alone");
        var fake = Path.Combine(Payload.Root, "0.0.0-unit-test");
        var inside = Path.Combine(fake, Payload.FilesDir);
        bool removed;
        try
        {
            Directory.CreateDirectory(inside);
            File.WriteAllText(Path.Combine(inside, "manifest.json"), "{}");
            Payload.Cleanup(inside);
            removed = !Directory.Exists(fake);
        }
        catch (UnauthorizedAccessException) { removed = true; }   // a test that may not write there proves the rest
        catch (IOException) { removed = true; }
        Check(removed, "single file: Cleanup removes the version folder it unpacked");
        try { Directory.Delete(tmp, true); } catch (Exception) { }
    }

    static void LocaleContracts()
    {
        var before = System.Threading.Thread.CurrentThread.CurrentCulture;
        var beforeUi = System.Threading.Thread.CurrentThread.CurrentUICulture;
        try
        {
            foreach (var name in new[] { "en-US", "pl-PL", "es-ES", "ja-JP", "ko-KR" })
            {
                System.Threading.Thread.CurrentThread.CurrentCulture = System.Globalization.CultureInfo.GetCultureInfo(name);
                System.Threading.Thread.CurrentThread.CurrentUICulture = System.Globalization.CultureInfo.GetCultureInfo(name);
                var args = SetupArgs.Parse(new[] { "--smoke-render", "d", "1.25", "en", "--text-scale", "1.5" });
                Check(args.Error == null && args.Scale == 1.25f && args.TextScale == 1.5f, name + ": invariant command-line decimals");
                Check(SetupArgs.Parse(new[] { "--smoke-render", "d", "1,25", "en" }).Error != null, name + ": comma decimal is not a machine argument");
                var json = Json.Parse("{\"fraction\":1.25,\"count\":42,\"enabled\":true}");
                Equal("1.25", Json.Str(json, "fraction"), name + ": JSON decimal stays invariant");
                Equal(42, Json.Int(json, "count"), name + ": JSON integer");
                Check(Json.Bool(json, "enabled"), name + ": JSON boolean");
            }
        }
        finally { System.Threading.Thread.CurrentThread.CurrentCulture = before; System.Threading.Thread.CurrentThread.CurrentUICulture = beforeUi; }
    }

    [DllImport("shell32.dll", SetLastError = true)]
    static extern IntPtr CommandLineToArgvW([MarshalAs(UnmanagedType.LPWStr)] string cmd, out int count);

    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr p);

    static void Quoting()
    {
        var args = new[] { "plain", "with space", @"C:\Program Files\x\", "quote\"inside", @"back\\slashes\", "", @"tail\\", "-EventsFile", @"C:\Users\a b\AppData\Local\Temp\amdgpu-wddm-setup\x\events.jsonl" };
        var line = "prog.exe " + CommandLine.Join(args);
        int n;
        var p = CommandLineToArgvW(line, out n);
        var back = new List<string>();
        for (int i = 1; i < n; i++) back.Add(Marshal.PtrToStringUni(Marshal.ReadIntPtr(p, i * IntPtr.Size)));
        LocalFree(p);
        Check(back.SequenceEqual(args), "quoting: CommandLineToArgvW reads back every argument (" + string.Join(" | ", back) + ")");
    }

    // TOKEN_PRIVILEGES with one LUID_AND_ATTRIBUTES as winnt.h lays it out (SDK 10.0.26100): PrivilegeCount at 0, the
    // LUID at 4, its Attributes at 12, 16 bytes. The planned restart enables SeShutdownPrivilege through it.
    static void NativeLayout()
    {
        var t = typeof(Native.TokenPrivileges);
        Equal(16, Marshal.SizeOf(t), "TOKEN_PRIVILEGES size");
        Equal(0, (int)Marshal.OffsetOf(t, "Count"), "TOKEN_PRIVILEGES PrivilegeCount offset");
        Equal(4, (int)Marshal.OffsetOf(t, "Luid"), "TOKEN_PRIVILEGES Privileges[0].Luid offset");
        Equal(12, (int)Marshal.OffsetOf(t, "Attributes"), "TOKEN_PRIVILEGES Privileges[0].Attributes offset");
    }

    // The support file goes through Redactor (SupportFile.Write), and run-<n>-diagnostics.txt of the installer is
    // one of its files: every line of it carries a prefix with a colon of its own (a setupapi line, an event line
    // with its timestamp, a record line), which the serial and machine-id rule has to see past. The same test is in
    // the control application, whose Redactor.cs this file is a copy of.
    static void Redaction()
    {
        var r = new Redactor("tester", "BENCH-PC", @"C:\Users\tester");
        Check(r.Apply("System Serial Number: ABC123XYZ") == "System Serial Number: <redacted>", "a serial at the start of the line");
        Check(r.Apply("     dvi: Device serial: SN-ABC123456789") == "     dvi: Device serial: <redacted>", "a serial behind a setupapi prefix");
        Check(r.Apply("  2026-10-12T01:59:48.123Z level 2 id 411 Device serial number: SN-ABC123456789")
            == "  2026-10-12T01:59:48.123Z level 2 id 411 Device serial number: <redacted>", "a serial in an event line of the diagnostics file");
        Check(r.Apply("StartRecord log: MachineGuid: 11111111-2222-3333-4444-555555555555")
            == "StartRecord log: MachineGuid: <redacted>", "a machine id in a record line");
        // One record of an event is ONE line of XML, so a keyword anywhere in it used to let the rule run
        // to the end of the line and take the message with it. The value of the element that names the
        // keyword goes; every other element of the same line stays.
        const string xml = "<Event><EventData><Data Name=\"AdapterUuid\">1234abcd</Data>"
            + "<Data Name=\"Reason\">the GPU was reset</Data></EventData></Event>";
        var oneLine = r.Apply(xml);
        Check(!oneLine.Contains("1234abcd"), "the UUID of a one-line event record goes");
        Check(oneLine.Contains("<Data Name=\"Reason\">the GPU was reset</Data></EventData></Event>"),
            "and the rest of that line, the reading the report was collected for, stays");
        Check(r.Apply("<Data Name=\"SerialNumber\">SN-ABC123456789</Data><Data Name=\"Code\">43</Data>")
            == "<Data Name=\"SerialNumber\"><redacted></Data><Data Name=\"Code\">43</Data>",
            "a serial in markup goes by itself, and the code beside it stays");
        Check(r.Apply("driver INF: oem42.inf") == "driver INF: oem42.inf", "a line that names no personal value is left alone");
        // The whole file at once, with the line ending it is written with: a rule anchored with $ matches
        // nothing before a carriage return, so the same lines in one CRLF text are the case that matters.
        var text = string.Join("\r\n", new[] {
            "schema amdgpu-wddm.diagnostics/1",
            "     dvi: Device serial: SN-ABC123456789",
            "  2026-10-12T01:59:48.123Z level 4 id 100 MachineGuid: 11111111-2222-3333-4444-555555555555",
            @"device instance: PCI\VEN_1002&DEV_13FE&SUBSYS_13FE1002&REV_C1",
        });
        var done = r.Apply(text);
        Check(!done.Contains("SN-ABC123456789") && !done.Contains("11111111-2222-3333-4444-555555555555"),
            "a CRLF text has its serial and its machine id removed");
        Check(done.Contains("Device serial: <redacted>\r\n") && done.Contains("MachineGuid: <redacted>\r\n"),
            "and keeps the label and the line ending");
        Check(done.Contains("PCI\\VEN_1002&DEV_13FE"), "while the hardware id of that text stays");
        Check(r.Apply(@"device instance: PCI\VEN_1002&DEV_13FE&SUBSYS_13FE1002&REV_C1")
            == @"device instance: PCI\VEN_1002&DEV_13FE&SUBSYS_13FE1002&REV_C1", "the hardware id of the report stays");
        // Train b31: a collected log can name ANOTHER account of the same computer. The control application's
        // rules take those out, and this file is a copy of that one, so the three cases are here as well.
        Check(r.Apply(@"      cmd: C:\Users\otherperson\Downloads\install.cmd")
            == @"      cmd: C:\Users\<user>\Downloads\install.cmd", "another account's profile name goes, and the rest of the path stays");
        Check(r.Apply("      sid: S-1-5-21-3623811015-3361044348-30300820-1013")
            == "      sid: <sid>", "a security identifier goes");
        Check(r.Apply("      <Computer>SOMEBODY-ELSE-PC</Computer>")
            == "      <Computer><computer></Computer>", "the computer of an event entry goes");
    }
}
