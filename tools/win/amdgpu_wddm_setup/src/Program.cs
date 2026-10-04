// amdgpu-wddm Setup: the setup window for the amdgpu-wddm driver (GUI plan v7 phase 1, WU-001, WU-044, WU-047,
// WU-048, WU-051, WU-058). Arguments: SetupArgs.cs. The window runs as the invoking user and starts itself again as
// administrator for an install, an update, a repair or a continuation (one UAC prompt); preparing an offline folder
// needs no administrator. The headless entries (--smoke-render, --smoke-engine) never show a window.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    public static class Program
    {
        public static string VersionText { get { return Assembly.GetExecutingAssembly().GetName().Version.ToString(); } }

        [STAThread]
        static int Main(string[] argv)
        {
            var a = SetupArgs.Parse(argv);
            if (a.Error != null) { Console.Error.WriteLine("amdgpu_wddm_setup: " + a.Error); return 2; }
            if (a.Mode == "version") { Console.WriteLine("amdgpu-wddm Setup " + VersionText); return 0; }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (a.Mode == "smoke-render") return SmokeRender(a);
            if (a.Mode == "smoke-engine") return SmokeEngine(a);
            if (a.Mode == "smoke-start-failure") return SmokeStartFailure(a);

            Strings.Language = Native.ChosenLanguage() ?? Strings.SystemLanguage();
            Theme.Fonts();
            if (a.Mode != "prepare-offline" && !Native.IsElevated())
            {
                if (Native.RelaunchElevated(argv)) return 0;
                MessageBox.Show(Strings.T("elevation.declined"), "amdgpu-wddm Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return 5;
            }
            Application.Run(new SetupForm(a, false));
            return 0;
        }

        // Every fixture screen drawn at one scale and language: <dir>\<screen>.png, <dir>\<screen>.txt (its text) and
        // <dir>\layout.txt (overlaps, overflow, G-A11Y and G-NOINT findings; "ok" when there are none).
        static int SmokeRender(SetupArgs a)
        {
            var layout = Path.Combine(a.SmokeDir, "layout.txt");
            try
            {
                Directory.CreateDirectory(a.SmokeDir);
                Strings.Language = a.Language;
                Theme.Init(a.Scale, a.TextScale);
                var w = new StringBuilder();
                int screens = 0;
                foreach (var f in Fixtures.All())
                {
                    using (var form = new SetupForm(a, true))
                    {
                        form.ShowFixture(f);
                        foreach (var line in form.Render(Path.Combine(a.SmokeDir, f.Name + ".png")).Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)) w.AppendLine(f.Name + ": " + line);
                        foreach (var p in form.Accessibility()) w.AppendLine(f.Name + ": G-A11Y " + p);
                        var text = form.VisibleText(false);
                        foreach (var hit in NoInternals.Find(text)) w.AppendLine(f.Name + ": G-NOINT \"" + hit + "\"");
                        if (text.Contains("[")) foreach (var m in System.Text.RegularExpressions.Regex.Matches(text, @"\[[a-z0-9.-]+\]")) w.AppendLine(f.Name + ": missing string " + m);
                        File.WriteAllText(Path.Combine(a.SmokeDir, f.Name + ".txt"), form.VisibleText(true), new UTF8Encoding(false));
                        screens++;
                    }
                }
                File.WriteAllText(layout, w.Length == 0 ? "ok: " + screens + " screens\r\n" : w.ToString(), new UTF8Encoding(false));
                return w.Length == 0 ? 0 : 1;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(layout, "render failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        // An engine that cannot start, for plan, install and prepare: a run folder that cannot be made (the run root is
        // under a file) and a package folder that is gone. Each must end on the result screen and stay there, never on
        // an endless Checking or Working screen. summary.txt: one line per case, then "ok" or the count of wrong ones.
        static int SmokeStartFailure(SetupArgs a)
        {
            var w = new StringBuilder();
            var summary = Path.Combine(a.SmokeDir, "summary.txt");
            int bad = 0;
            try
            {
                Directory.CreateDirectory(a.SmokeDir);
                Strings.Language = "en";
                var file = Path.Combine(a.SmokeDir, "run-root-is-a-file");
                File.WriteAllText(file, "not a folder");
                var gone = Path.Combine(a.SmokeDir, "package-that-is-gone");
                foreach (var cause in new[] { "run-root", "package" })
                    foreach (var kind in new[] { "plan", "install", "prepare" })
                    {
                        var root = cause == "run-root" ? Path.Combine(file, "runs") : Path.Combine(a.SmokeDir, "runs");
                        var b = SetupArgs.Parse(new[] { "--run-root", root, "--package", gone });
                        using (var form = new SetupForm(b, true))
                        {
                            form.StartForTest(kind);
                            var v = form.View;
                            var ok = form.Current == Screen.Result && v != null && v.Kind == ViewKind.Problem && v.ChangesUnknown;
                            if (!ok) bad++;
                            w.AppendLine(kind + " " + cause + ": " + form.Current + " " + (v != null ? v.Kind.ToString() : "-") + (ok ? "" : " WRONG"));
                        }
                    }
                w.AppendLine(bad == 0 ? "ok" : bad + " wrong");
                File.WriteAllText(summary, w.ToString(), new UTF8Encoding(false));
                return bad == 0 ? 0 : 1;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(summary, w + "failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        // One engine run without a window: the client, the model and the screen the window would show (in all four
        // languages, checked for internals), and the support file. summary.txt is what the build and the release
        // tests read.
        static int SmokeEngine(SetupArgs a)
        {
            var w = new StringBuilder();
            var summary = Path.Combine(a.SmokeDir, "summary.txt");
            try
            {
                Directory.CreateDirectory(a.SmokeDir);
                Strings.Language = "en";
                var client = new EngineClient(a.SmokeDir, false);
                client.Start(Path.Combine(Path.GetFullPath(a.Package), "installer", a.Script), a.EngineArgs);
                var clock = Stopwatch.StartNew();
                bool cancelAsked = false, timedOut = false;
                while (!client.Finished)
                {
                    client.Poll();
                    if (a.Cancel && !cancelAsked && client.Run.CancelAvailable) cancelAsked = client.RequestCancel();
                    if (clock.Elapsed > TimeSpan.FromMinutes(15)) { timedOut = true; break; }
                    Thread.Sleep(100);
                }
                var run = client.Run;
                var result = client.Result;
                var view = ResultView.For(result, run);
                w.AppendLine("invocation: " + client.Invocation);
                w.AppendLine("exit: " + (client.ExitCode.HasValue ? client.ExitCode.Value.ToString() : timedOut ? "timeout" : "-"));
                w.AppendLine("result: " + (result != null ? "bound" : "none (" + client.ResultProblem + ")"));
                if (result != null)
                {
                    w.AppendLine("outcome: " + result.Outcome);
                    w.AppendLine("message: " + result.MessageId);
                    w.AppendLine("mutated: " + result.Mutated.ToString().ToLowerInvariant());
                    w.AppendLine("restart: " + (result.RestartRequired ? result.RestartReason : "no"));
                }
                w.AppendLine("view: " + view.Kind + " " + view.TitleId + (view.NothingChanged ? " nothing-changed" : "") + (view.ChangesMade ? " changes-made" : "") + (view.ChangesUnknown ? " changes-unknown" : "") +
                    (view.OfferRestart ? " offer-restart" : "") + (view.OfferRetry ? " offer-retry" : "") + (view.OfferRepair ? " offer-repair" : ""));
                w.AppendLine("events: " + run.LastSeq + " ignored " + run.Ignored + " problems " + run.Problems.Count + (run.Problems.Count > 0 ? " (" + string.Join("; ", run.Problems) + ")" : ""));
                w.AppendLine("mode: " + run.Mode);
                w.AppendLine("stages: " + string.Join(",", run.Stages));
                w.AppendLine("checks: " + string.Join(",", run.Checks.Select(c => c.Id + ":" + c.Result)));
                w.AppendLine("unknown checks: " + string.Join(",", run.Checks.Where(c => c.Id == null || !Strings.Has("check." + c.Id)).Select(c => c.Id ?? "-")));
                if (run.Decision != null)
                    w.AppendLine("decision: " + run.Decision.Action + " restarts " + run.Decision.Restarts + " consents " + string.Join("+", run.Decision.Consents) +
                        " firmware " + run.Decision.FirmwareSource + " notes " + string.Join("+", run.Decision.Notes) + " compatibility " + (run.Decision.CompatibilityOk ? "ok" : "no"));
                if (run.Settings != null)
                    w.AppendLine("settings: rows " + run.Settings.Rows.Count + " lines " + SettingsView.Lines(run.Settings).Count + " kept " + run.Settings.Kept + " added " + run.Settings.Added + " updated " + run.Settings.Updated);
                w.AppendLine("cancel: " + (cancelAsked ? "requested" : "not requested"));
                w.AppendLine("steps: " + run.Steps + " real " + run.RealSteps + (run.RestartReason != null ? "; restart event " + run.RestartReason : ""));

                using (var form = new SetupForm(a, true))
                {
                    form.Adopt(client, a.PlanRun ? "plan" : a.Script == "prepare-offline.ps1" ? "prepare" : "install");
                    foreach (var lang in Strings.Languages)
                    {
                        Strings.Language = lang;
                        var text = form.VisibleText(false);
                        var hits = NoInternals.Find(text);
                        var missing = System.Text.RegularExpressions.Regex.Matches(text, @"\[[a-z0-9.-]+\]").Cast<System.Text.RegularExpressions.Match>().Select(m => m.Value).ToList();
                        w.AppendLine("screen " + lang + ": " + form.Current + "; internals " + (hits.Count == 0 ? "none" : string.Join(",", hits)) + "; missing strings " + (missing.Count == 0 ? "none" : string.Join(",", missing)));
                        if (lang == "en") w.AppendLine("text en: " + text.Replace("\r\n", " | ").Replace("\n", " | "));
                    }
                    Strings.Language = "en";
                    var zip = Path.Combine(a.SmokeDir, "support.zip");
                    w.AppendLine("support: " + (form.WriteSupport(zip) ? zip : "failed"));
                }
                File.WriteAllText(summary, w.ToString(), new UTF8Encoding(false));
                return 0;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(summary, w + "smoke-engine failed: " + e); } catch (Exception) { }
                return 1;
            }
        }
    }
}
