// amdgpu-wddm Control: the tester's control application for the amdgpu-wddm driver on the ASRock BC-250.
//
//   amdgpu_wddm_control.exe                          the window
//   amdgpu_wddm_control.exe --smoke <file>           no window: build the pages once, write what they show, exit
//   amdgpu_wddm_control.exe --smoke-report <zip>    no window: a bug report without dxdiag, the capability tools and
//                                                    the event logs, written to <zip> (the build's check of that path)
//   amdgpu_wddm_control.exe --version
//   (internal, elevated copy) --write-dpm <mode> <MHz> | --write-profile <image> <list> | --remove-profile <image>
//
// Runs as the invoking user. A settings change starts an elevated copy of this program with one --write-* verb,
// which validates its arguments with the same functions as the window and exits with 0 on success.
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
            if (args.Length > 0 && args[0].StartsWith("--write-") || args.Length > 0 && args[0] == "--remove-profile") return Write(args);
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            if (args.Length == 2 && args[0] == "--smoke") return Smoke(args[1]);
            if (args.Length == 2 && args[0] == "--smoke-report") return SmokeReport(args[1]);
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

        static int Write(string[] args)
        {
            try
            {
                uint a, b;
                if (args[0] == "--write-dpm" && args.Length == 3 && uint.TryParse(args[1], out a) && uint.TryParse(args[2], out b))
                    SettingsStore.WriteDpm(a, b);
                else if (args[0] == "--write-profile" && args.Length == 3)
                    SettingsStore.WriteProfile(args[1], args[2]);
                else if (args[0] == "--remove-profile" && args.Length == 2)
                    SettingsStore.RemoveProfile(args[1]);
                else return 2;
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

        // One settings write as administrator. In this process when it is elevated already, else through an elevated
        // copy (one UAC prompt per change). Returns null on success, else the reason.
        public static string RunElevated(params string[] verb)
        {
            if (IsElevated()) return Write(verb) == 0 ? null : "The change was refused. Check the values and try again.";
            var psi = new ProcessStartInfo(Assembly.GetExecutingAssembly().Location, string.Join(" ", Array.ConvertAll(verb, Quote)))
            {
                UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden,
            };
            try
            {
                using (var p = Process.Start(psi))
                {
                    p.WaitForExit();
                    return p.ExitCode == 0 ? null : "The change was refused (exit code " + p.ExitCode + ").";
                }
            }
            catch (System.ComponentModel.Win32Exception) { return "Administrator permission was not given. Nothing was changed."; }
        }
    }
}
