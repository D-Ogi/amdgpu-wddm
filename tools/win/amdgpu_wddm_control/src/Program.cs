// amdgpu-wddm Control: the tester's control application for the amdgpu-wddm driver on the ASRock BC-250.
//
//   amdgpu_wddm_control.exe                          the window
//   amdgpu_wddm_control.exe --smoke <file>           no window: build the pages once, write what they show, exit
//   amdgpu_wddm_control.exe --smoke-report <zip>    no window: a bug report without dxdiag, the capability tools and
//                                                    the event logs, written to <zip> (the build's check of that path)
//   amdgpu_wddm_control.exe --smoke-render <dir> <scale>
//                                                    no window: every page drawn to <dir>\<page>.png as a screen at
//                                                    <scale> x 96 DPI shows it, and layout.txt lists overlapping controls
//   amdgpu_wddm_control.exe --action <name> [--ceiling MHz] --dry-run [--snapshot <json>] [--out <file>]
//                                                    no window: the Recovery states and the plan of one action, nothing
//                                                    written (RecoveryActions.cs)
//   amdgpu_wddm_control.exe --status [--out <file>]  no window: the Recovery states, first a "compositor-restarted:"
//                                                    line (BD-060) for the installer's verify; nothing written
//   amdgpu_wddm_control.exe --version
//   (internal, elevated copy) --action <name> ... | --apply-profiles <image> <list or empty to remove> ...
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
            if (args.Length > 0 && args[0] == "--status") return Status(args);
            if (args.Length > 0 && (args[0] == "--action" || args[0] == "--apply-profiles")) return Dispatch(args);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (args.Length == 2 && args[0] == "--smoke") return Smoke(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-report") return SmokeReport(args[1]);
            if (args.Length == 3 && args[0] == "--smoke-render") return SmokeRender(args[1], args[2]);
            if (args.Length != 0) { MessageBox.Show("Unknown arguments.", ProductName); return 2; }
            Application.Run(new MainForm());
            return 0;
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

        static int SmokeRender(string dir, string scale)
        {
            var layout = Path.Combine(dir, "layout.txt");
            try
            {
                Directory.CreateDirectory(dir);
                Theme.Init(float.Parse(scale, System.Globalization.CultureInfo.InvariantCulture));
                string overlaps;
                using (var form = new MainForm(smoke: true))
                {
                    form.CreateControlTree();
                    form.RefreshAll();
                    overlaps = form.RenderPages(dir);
                }
                File.WriteAllText(layout, overlaps.Length == 0 ? "no overlapping controls\r\n" : overlaps);
                return overlaps.Length == 0 ? 0 : 1;
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
                    Environment.NewLine + RecoveryProbe.StatusText(RecoveryProbe.Read());
            }
            catch (Exception e) { text = "status failed: " + e.Message + Environment.NewLine; }
            if (args.Length == 3) File.WriteAllText(args[2], text);
            else Console.Out.Write(text);
            return text.StartsWith("status failed", StringComparison.Ordinal) ? 1 : 0;
        }

        static int Dispatch(string[] args)
        {
            if (args[0] == "--action") return RecoveryRunner.Run(args);
            // --apply-profiles <image> <value> ...: every pair checked before the first write; an empty value removes
            // the application's key (no empty value is ever stored).
            if (args.Length < 3 || args.Length % 2 != 1) return 2;
            for (int i = 1; i < args.Length; i += 2)
                if (!Profiles.IsValidImage(args[i]) || args[i + 1].Length > 0 && !Profiles.IsValidValue(args[i + 1])) return 2;
            try
            {
                for (int i = 1; i < args.Length; i += 2)
                    if (args[i + 1].Length == 0) SettingsStore.RemoveProfile(args[i]);
                    else SettingsStore.WriteProfile(args[i], args[i + 1]);
                return 0;
            }
            catch (Exception) { return 1; }
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
