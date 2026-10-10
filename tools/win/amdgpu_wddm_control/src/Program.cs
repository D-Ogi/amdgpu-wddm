// amdgpu-wddm Control: the tester's control application for the amdgpu-wddm driver on the ASRock BC-250.
//
//   amdgpu_wddm_control.exe                          the window
//   amdgpu_wddm_control.exe --smoke <file>           no window: build the pages once, write what they show, exit
//   amdgpu_wddm_control.exe --smoke-report <zip>    no window: a bug report without dxdiag, the capability tools and
//                                                    the event logs, written to <zip> (the build's check of that path)
//   amdgpu_wddm_control.exe --smoke-render <dir> <scale> [--lang xx] [--text-scale f] [--nagi] [--switch-to xx]
//                                [--fixture <snapshot.json>]
//                                                    no window: every page drawn to <dir>\<page>.png as a screen at
//                                                    <scale> x 96 DPI shows it; layout.txt lists what the render gates
//                                                    found (overlap, overflow, internals, accessible names)
//   amdgpu_wddm_control.exe --recovery               the recovery view: no bc250control.dll, no driver questions
//   amdgpu_wddm_control.exe --smoke-recovery <file>  no window: the recovery view built; fails when the DLL got loaded
//   amdgpu_wddm_control.exe --smoke-perf <file>      no window: start time, timer and frame ticks of a hidden window,
//                                                    memory (G-PERF)
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
        public const string ProductName = "amdgpu-wddm Control";

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
            if (args.Length == 1 && args[0] == "--version") { Console.WriteLine(ProductName + " " + VersionText); return 0; }
            if (args.Length > 0 && args[0] == "--bc250-board-memory-action") return UmaActions.Run(args);
            if (args.Length > 0 && args[0] == "--status") return Status(args);
            if (args.Length > 0 && args[0] == "--action") return Dispatch(args);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (args.Length == 2 && args[0] == "--smoke") return Smoke(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-report") return SmokeReport(args[1]);
            if (args.Length >= 3 && args[0] == "--smoke-render") return SmokeRender(args);
            if (args.Length == 2 && args[0] == "--smoke-recovery") return SmokeRecovery(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-perf") return SmokePerf(args[1]);
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

        static int SmokeRender(string[] args)
        {
            string dir = args[1];
            var layout = Path.Combine(dir, "layout.txt");
            try
            {
                Directory.CreateDirectory(dir);
                var inv = System.Globalization.CultureInfo.InvariantCulture;
                float scale = float.Parse(args[2], inv), textScale = 1;
                string lang = "en", fixture = null;
                var o = new MainForm.RenderOptions { Directory = dir };
                for (int i = 3; i < args.Length; i++)
                {
                    if (args[i] == "--lang" && i + 1 < args.Length) lang = args[++i];
                    else if (args[i] == "--text-scale" && i + 1 < args.Length) textScale = float.Parse(args[++i], inv);
                    else if (args[i] == "--nagi") o.Nagi = true;
                    else if (args[i] == "--switch-to" && i + 1 < args.Length) o.SwitchTo = args[++i];
                    else if (args[i] == "--fixture" && i + 1 < args.Length) fixture = args[++i];
                    else throw new ArgumentException("unknown option " + args[i]);
                }
                if (Array.IndexOf(Strings.Languages, lang) < 0) throw new ArgumentException("unknown language " + lang);
                var prefs = new AppPrefs(new MemoryPrefStore()) { Language = lang, ShowNagi = o.Nagi };
                Strings.Language = lang;
                Theme.Init(scale, textScale);
                string problems;
                using (var form = new MainForm(true, prefs))
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
                report.Collect(false, false, false, _ => { });
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
