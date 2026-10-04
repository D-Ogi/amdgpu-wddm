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
            Settings();
            Notes(repo);
            Prepared();
            Guide();
            Arguments();
            Quoting();
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
        foreach (var d in new[] { "set", "same", "update", "kept", "command" }) Check(Strings.Has("settings.decision." + d), "settings: decision " + d + " has words");
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
        var dir = Path.Combine(Path.GetTempPath(), "amdgpu-wddm-setup-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            File.WriteAllText(Path.Combine(dir, "RELEASE-NOTES.md"), md);
            var n = ReleaseNotes.Load(dir, "ja");
            Check(n.EnglishFallback && n.Language == "en", "C16: no Japanese notes: English, labelled");
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
    }

    static void Arguments()
    {
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
}
