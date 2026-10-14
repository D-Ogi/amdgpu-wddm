// Nagi Control App: the tester's control application for the Nagi graphics driver (amdgpu-wddm) on the ASRock
// BC-250. The display name is Nagi (owner, 2026-10-13); the file, registry and folder names keep amdgpu-wddm.
//
//   amdgpu_wddm_control.exe                          the window
//   amdgpu_wddm_control.exe --smoke <file>           no window: build the pages once, write what they show, exit
//   amdgpu_wddm_control.exe --smoke-report <zip>    no window: a bug report without dxdiag, the capability tools and
//                                                    the event logs, written to <zip> (the build's check of that path)
//   amdgpu_wddm_control.exe --smoke-render <dir> <scale> [--lang xx] [--text-scale f] [--no-nagi] [--switch-to xx]
//                                [--fixture <snapshot.json>]
//                                                    no window: every page drawn to <dir>\<page>.png as a screen at
//                                                    <scale> x 96 DPI shows it; layout.txt lists what the render gates
//                                                    found (overlap, overflow, internals, accessible names)
//   amdgpu_wddm_control.exe --start-notice           the sign-in notice: reads the driver's start, shows a window
//                                                    only when the driver is not running (StartNotice.cs), else
//                                                    exits 0 with nothing on the screen
//   amdgpu_wddm_control.exe --smoke-notice <file>    no window: the notice built in every case, language and scale,
//                                                    with its layout gate; findings go to <file> (BD-137)
//   amdgpu_wddm_control.exe --recovery               the recovery view: no bc250control.dll, no driver questions
//   amdgpu_wddm_control.exe --smoke-recovery <file>  no window: the recovery view built; fails when the DLL got loaded
//   amdgpu_wddm_control.exe --smoke-perf <file>      no window: start time, timer and frame ticks of a hidden window,
//                                                    memory (G-PERF)
//   amdgpu_wddm_control.exe --smoke-scroll <file> --fixture <snapshot.json>
//                                                    no window: a rebuilt page keeps the place the person scrolled
//                                                    to, and the buttons of a benchmark run stay where they are in
//                                                    every state of the run; findings go to <file>
//   amdgpu_wddm_control.exe --action <name> [--ceiling MHz] --dry-run [--snapshot <json>] [--out <file>]
//                                                    no window: the Recovery states and the plan of one action, nothing
//                                                    written (RecoveryActions.cs)
//   amdgpu_wddm_control.exe --status [--out <file>]  no window: the Recovery states, first a "dwm-restart:" line
//                                                    (BD-060) for the installer's verify; records the session's DWM
//   amdgpu_wddm_control.exe --version
//   (internal, elevated copy) --action <name> ...    one planned change (game settings: --action game-profile)
//
// Runs as the invoking user. A change starts an elevated copy of this program with one verb (one UAC prompt per
// change), which plans or validates again with the same functions as the window and exits with 0 on success.
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Principal;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public static class Program
    {
        public const string ProductName = "Nagi Control App";

        public static string VersionText
        {
            get { return Assembly.GetExecutingAssembly().GetName().Version.ToString(); }
        }

        public static string AppDirectory
        {
            get { return Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location); }
        }

        [STAThread]
        static int Main(string[] args)
        {
            // BD-174: a process-fatal exception leaves a record of its own, because a machine with Windows Error
            // Reporting switched off writes none. It changes nothing else (Crash.cs).
            Crash.Watch();
            if (args.Length == 1 && args[0] == "--startup-diagnostics-worker") return StartupDiagnostics.Worker();
            if (args.Length == 1 && args[0] == "--version") { Console.WriteLine(ProductName + " " + VersionText); return 0; }
            if (args.Length > 0 && args[0] == "--bc250-board-memory-action") return UmaActions.Run(args);
            if (args.Length > 0 && args[0] == "--status") return Status(args);
            if (args.Length > 0 && args[0] == "--action") return Dispatch(args);
            if (args.Length > 0 && args[0] == "--system-tuning") return Dispatch(args);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (args.Length == 2 && args[0] == "--smoke") return Smoke(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-report") return SmokeReport(args[1]);
            if (args.Length >= 3 && args[0] == "--smoke-render") return SmokeRender(args);
            if (args.Length == 2 && args[0] == "--smoke-recovery") return SmokeRecovery(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-perf") return SmokePerf(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-notice") return SmokeNotice(args[1]);
            if (args.Length == 1 && args[0] == "--start-notice") return StartNoticeRun();
            if ((args.Length == 4 || args.Length == 6 && args[4] == "--board-fixture") && args[0] == "--smoke-scroll" && args[2] == "--fixture")
                return SmokeScroll(args[1], args[3], args.Length == 6 ? args[5] : null);
            if (args.Length == 1 && args[0] == "--recovery")
            {
                Kmd.Blocked = true;
                Strings.Language = AppPrefs.Live().EffectiveLanguage;
                Theme.Fonts();
                Application.Run(new RecoveryView());
                return 0;
            }
            if (args.Length != 0) { MessageBox.Show("Unknown arguments.", ProductName); return 2; }
            Application.Run(new MainForm());
            return 0;
        }

        // ---- the sign-in notice (StartNotice.cs, NoticeForm.cs) ------------------------------------------------
        // What the machine says about the driver's start, with no state file and no help from the task that starts
        // this: the driver's settings key is readable by any account, and a reading of the driver's own state needs
        // no rights either (only a confirmation does, driver/kmd/start_health.c). So the person's own session can
        // answer both questions by itself, which is why the logon window task keeps its rule that it touches
        // neither the driver nor the registry: it starts this process and nothing more.
        static StartReading ReadStart()
        {
            var r = new StartReading();
            try
            {
                using (var key = SettingsStore.Machine.OpenSubKey(Recovery.ParametersPath))
                {
                    r.Read = true;
                    r.Installed = key != null;
                    if (key != null)
                    {
                        var starts = key.GetValue("UnconfirmedStarts") as int?;
                        var stage = key.GetValue("LastStage") as int?;
                        r.UnconfirmedStarts = starts == null ? null : (uint?)(uint)starts.Value;
                        r.LastStage = stage == null ? null : (uint?)(uint)stage.Value;
                    }
                }
            }
            catch (Exception e) { r.Read = false; r.DriverError = e.Message; return r; }
            var health = Kmd.StartHealth();
            r.Running = health.Value != null;
            r.DriverError = health.Value != null ? null : health.Error;
            return r;
        }

        // The notice's own run. It polls, because the logon starts this at the same moment as the driver's start:
        // a driver that is still starting answers nothing, and a notice from that moment would be false. A driver
        // that answers at any poll ends the run with nothing on the screen.
        static int StartNoticeRun()
        {
            Strings.Language = AppPrefs.Live().EffectiveLanguage;
            Theme.Fonts();
            var clock = Stopwatch.StartNew();
            var decided = StartNoticeCase.None;
            while (true)
            {
                var reading = ReadStart();
                decided = StartNotice.Decide(reading);
                if (decided == StartNoticeCase.None)
                {
                    // The driver runs: the Nagi wallpaper of a verified install, once per install, for the person
                    // who signed in (Wallpaper.cs). No window; it waits for the verification of this logon.
                    NagiWallpaper.RunAtSignIn(AppPrefs.Live());
                    return 0;
                }
                // A refused start is already decided: the driver wrote its refusal into the registry and nothing
                // but a start of the device can change it, so there is nothing left to wait for. The person is
                // told at once (owner, 2026-10-13: "Uzytkownik musi byc tego swiadomym od razu"). Only the other
                // case waits out the poll, because that is the case a driver still inside its own start looks
                // like, and a notice from that moment would be false.
                if (decided == StartNoticeCase.Guard) break;
                if (clock.Elapsed.TotalSeconds >= StartNotice.ProbeSeconds) break;
                System.Threading.Thread.Sleep(StartNotice.PollMs);
            }
            Application.Run(new NoticeForm(decided));
            return 0;
        }

        // BD-137: the notice built in both cases, in the four languages, at 96, 120, 144 and 192 DPI, for a
        // 1024x768 screen and for a large one. No window is shown. An empty findings file is a pass.
        static int SmokeNotice(string path)
        {
            try
            {
                var w = new System.Text.StringBuilder();
                var screens = new[] { new System.Drawing.Size(1024, 728), new System.Drawing.Size(1920, 1032) };
                int built = 0;
                // The text scale of Windows ("Make text bigger", Accessibility) belongs in this matrix as it does
                // in the page render gate: it grows the fonts without growing anything measured in pixels, so it
                // is the setting that makes a button's words outgrow the row that holds them. 225 % is the most
                // Windows offers, and a person stuck on a slow basic display is a plausible user of it.
                foreach (var lang in Strings.Languages)
                    foreach (var scale in new[] { 1f, 1.25f, 1.5f, 2f })
                        foreach (var textScale in new[] { 1f, 1.5f, 2.25f })
                            foreach (var screen in screens)
                                foreach (var c in new[] { StartNoticeCase.Guard, StartNoticeCase.Failed })
                                {
                                    Strings.Language = lang;
                                    Theme.Init(scale, textScale);
                                    w.Append(NoticeForm.Findings(c, screen));
                                    built++;
                                }
                string text = w.ToString();
                File.WriteAllText(path, text.Length == 0 ? "no findings, " + built + " windows built" + Environment.NewLine : text);
                return text.Length == 0 ? 0 : 1;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(path, "smoke notice failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        static bool DllLoaded()
        {
            using (var p = Process.GetCurrentProcess())
                foreach (ProcessModule m in p.Modules)
                    if (string.Equals(Path.GetFileName(m.FileName), "bc250control.dll", StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        static int SmokeRecovery(string path)
        {
            try
            {
                Kmd.Blocked = true;
                string text;
                using (var view = new RecoveryView())
                {
                    var h = view.Handle;
                    text = view.Describe();
                }
                bool loaded = DllLoaded();
                File.WriteAllText(path, (loaded ? "FAIL: bc250control.dll was loaded" : "dll: not loaded") + Environment.NewLine + text);
                return loaded ? 1 : 0;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(path, "smoke recovery failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        // G-PERF: the window built and refreshed as at a start, never shown; then 5 s of message loop, in which a hidden
        // window must not tick (no live timer, no character frames). Start time and memory are written for the bound.
        static int SmokePerf(string path)
        {
            try
            {
                var w = new System.Text.StringBuilder();
                var clock = Stopwatch.StartNew();
                using (var form = new MainForm(false, new AppPrefs(new MemoryPrefStore())))
                {
                    form.ReadOnlyProbe = true;
                    form.CreateControlTree();
                    form.RefreshAll();
                    long startMs = clock.ElapsedMilliseconds;
                    var loop = Stopwatch.StartNew();
                    while (loop.ElapsedMilliseconds < 5000) { Application.DoEvents(); System.Threading.Thread.Sleep(20); }
                    long mb;
                    using (var p = Process.GetCurrentProcess()) mb = p.PrivateMemorySize64 >> 20;
                    w.AppendLine("start-ms: " + startMs);
                    w.AppendLine("timer-running: " + form.TimerRunning);
                    w.AppendLine("ticks: " + form.Ticks);
                    w.AppendLine("frame-timer: " + form.FrameTimerRunning);
                    w.AppendLine("private-mb: " + mb);
                    bool ok = !form.TimerRunning && form.Ticks == 0 && !form.FrameTimerRunning && startMs < 5000 && mb < 300;
                    w.AppendLine(ok ? "ok" : "FAIL");
                    File.WriteAllText(path, w.ToString());
                    return ok ? 0 : 1;
                }
            }
            catch (Exception e)
            {
                try { File.WriteAllText(path, "smoke perf failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        // The place a rebuild of the same page keeps (F1 of the b23 lab round), without a window. Findings go to
        // <file>; an empty file is a pass.
        static int SmokeScroll(string path, string fixture, string boardFixture = null)
        {
            try
            {
                if (boardFixture != null) BoardCapabilities.Replace(BoardCapabilities.Parse(File.ReadAllBytes(boardFixture)));
                string findings;
                using (var form = new MainForm(true, new AppPrefs(new MemoryPrefStore())))
                {
                    form.CreateControlTree();
                    form.UseFixture(new System.Web.Script.Serialization.JavaScriptSerializer().Deserialize<RecoverySnapshot>(File.ReadAllText(fixture)));
                    findings = form.ScrollKeeping() + form.RunStateLayout();
                }
                File.WriteAllText(path, findings.Length == 0 ? "no findings" + Environment.NewLine : findings);
                return findings.Length == 0 ? 0 : 1;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(path, "smoke scroll failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        // The window's pages built and refreshed once without being shown; their text goes to a file. For a build
        // gate on a PC with or without a BC-250. Nothing is written to the registry.
        static int Smoke(string path)
        {
            try
            {
                using (var form = new MainForm(smoke: true))
                {
                    form.CreateControlTree();
                    form.RefreshAll();
                    File.WriteAllText(path, form.Describe());
                }
                return 0;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(path, "smoke failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        // "<width>x<height>" in logical units, as --work-area takes it.
        static System.Drawing.Size ParseSize(string text)
        {
            var parts = (text ?? "").Split('x');
            int w, h;
            if (parts.Length != 2 || !int.TryParse(parts[0], out w) || !int.TryParse(parts[1], out h) || w < 1 || h < 1)
                throw new ArgumentException("a work area is <width>x<height>, not " + text);
            return new System.Drawing.Size(w, h);
        }

        static int SmokeRender(string[] args)
        {
            string dir = args[1];
            var layout = Path.Combine(dir, "layout.txt");
            try
            {
                Directory.CreateDirectory(dir);
                var inv = System.Globalization.CultureInfo.InvariantCulture;
                float scale = float.Parse(args[2], inv), textScale = 1;
                string lang = "en", fixture = null, boardFixture = null;
                var o = new MainForm.RenderOptions { Directory = dir };
                for (int i = 3; i < args.Length; i++)
                {
                    if (args[i] == "--lang" && i + 1 < args.Length) lang = args[++i];
                    else if (args[i] == "--text-scale" && i + 1 < args.Length) textScale = float.Parse(args[++i], inv);
                    else if (args[i] == "--nagi") o.Nagi = true;                  // the default; kept for older callers
                    else if (args[i] == "--no-nagi") o.Nagi = false;
                    else if (args[i] == "--switch-to" && i + 1 < args.Length) o.SwitchTo = args[++i];
                    else if (args[i] == "--fixture" && i + 1 < args.Length) fixture = args[++i];
                    else if (args[i] == "--board-fixture" && i + 1 < args.Length) boardFixture = args[++i];
                    // The screen the window is drawn for, in logical units: the small-screen rule reads it instead of
                    // the screen of the PC the render runs on, so one render is the same picture everywhere.
                    else if (args[i] == "--work-area" && i + 1 < args.Length) o.WorkArea = ParseSize(args[++i]);
                    else throw new ArgumentException("unknown option " + args[i]);
                }
                if (Array.IndexOf(Strings.Languages, lang) < 0) throw new ArgumentException("unknown language " + lang);
                if (boardFixture != null && fixture == null) throw new ArgumentException("a board fixture requires a render snapshot");
                // Explicit render-only fixture. Production admission never reads a file or environment override.
                if (boardFixture != null) BoardCapabilities.Replace(BoardCapabilities.Parse(File.ReadAllBytes(boardFixture)));
                var prefs = new AppPrefs(new MemoryPrefStore()) { Language = lang, ShowNagi = o.Nagi };
                Strings.Language = lang;
                Theme.Init(scale, textScale);
                string problems;
                using (var form = new MainForm(true, prefs, o.WorkArea))
                {
                    form.CreateControlTree();
                    if (fixture != null) form.UseFixture(new System.Web.Script.Serialization.JavaScriptSerializer().Deserialize<RecoverySnapshot>(File.ReadAllText(fixture)));
                    else form.RefreshAll();
                    problems = form.RenderPages(o);
                }
                File.WriteAllText(layout, problems.Length == 0 ? "no findings\r\n" : problems);
                return problems.Length == 0 ? 0 : 1;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(layout, "render failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        static int SmokeReport(string path)
        {
            try
            {
                var report = new BugReport(Redactor.ForThisPc());
                report.Collect(false, false, false, _ => { }, false);
                report.Write(path);
                return 0;
            }
            catch (Exception e)
            {
                try { File.WriteAllText(path + ".error.txt", "smoke report failed: " + e); } catch (Exception) { }
                return 1;
            }
        }

        static int Status(string[] args)
        {
            if (!(args.Length == 1 || args.Length == 3 && args[1] == "--out")) { Console.Error.WriteLine("usage: --status [--out file]"); return 2; }
            string text;
            try
            {
                text = ProductName + " " + VersionText + " status, " + DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss'Z'", System.Globalization.CultureInfo.InvariantCulture) +
                    Environment.NewLine + RecoveryProbe.StatusText(RecoveryProbe.Read("status"));
            }
            catch (Exception e) { text = "status failed: " + e.Message + Environment.NewLine; }
            if (args.Length == 3) File.WriteAllText(args[2], text);
            else Console.Out.Write(text);
            return text.StartsWith("status failed", StringComparison.Ordinal) ? 1 : 0;
        }

        static int Dispatch(string[] args)
        {
            if (args.Length > 0 && args[0] == "--bc250-board-memory-action") return UmaActions.Run(args);
            if (args.Length > 0 && args[0] == "--system-tuning") return SystemTuningProcess.Dispatch(args);
            // Existing recovery verbs. Game settings go through --action game-profile (planned, backed up, undoable); the
            // old unplanned --apply-profiles verb is gone.
            return args.Length > 0 && args[0] == "--action" ? RecoveryRunner.Run(args) : RecoveryRunner.Usage;
        }

        public static bool IsElevated()
        {
            using (var id = WindowsIdentity.GetCurrent())
                return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
        }

        static string Quote(string s) { return "\"" + s.Replace("\"", "") + "\""; }

        public const int NotElevated = -1;

        // One change as administrator. In this process when it is elevated already, else through an elevated copy (one
        // UAC prompt per change). Returns the verb's exit code, NotElevated when the prompt was declined.
        public static int RunElevatedCode(params string[] verb)
        {
            if (IsElevated()) return Dispatch(verb);
            var psi = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, string.Join(" ", Array.ConvertAll(verb, Quote)))
            {
                UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden,
            };
            try
            {
                using (var p = Process.Start(psi))
                {
                    if (verb.Length > 0 && verb[0] == "--system-tuning")
                    {
                        // Its backend has its own 120 s deadline. A delayed child does not freeze the window.
                        if (!p.WaitForExit(150000)) return SystemTuningProcess.Timeout;
                        return p.ExitCode;
                    }
                    p.WaitForExit();
                    return p.ExitCode;
                }
            }
            catch (System.ComponentModel.Win32Exception) { return NotElevated; }
        }

        // Returns null on success, else the reason.
        public static string RunElevated(params string[] verb)
        {
            int code = RunElevatedCode(verb);
            return code == 0 ? null : code == NotElevated ? "Administrator permission was not given. Nothing was changed." : "The change was refused (exit code " + code + ").";
        }
    }
}
