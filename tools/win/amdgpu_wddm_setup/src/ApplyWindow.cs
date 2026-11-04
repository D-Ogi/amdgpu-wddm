// The window of setup.exe --apply (ApplyUpdate.cs holds the pure part): one small window while installer\apply.ps1
// runs, then one message with the outcome, then the setup window of the new release for a driver update.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    public static class ApplyWindow
    {
        public static int Run(SetupArgs a, string[] argv)
        {
            if (!Native.IsElevated())
            {
                // The control application starts this as administrator; a copy started otherwise asks once.
                using (var p = Native.RelaunchElevated(argv, IntPtr.Zero))
                {
                    if (p == null) return 5;
                    p.WaitForExit();
                    return p.ExitCode;
                }
            }
            if (a.WaitPid > 0)
            {
                // The control application closes itself after it started this, so that its files can be switched.
                try { using (var p = Process.GetProcessById(a.WaitPid)) p.WaitForExit(ApplyUpdate.WaitPidSeconds * 1000); }
                catch (ArgumentException) { }
                catch (InvalidOperationException) { }
            }
            var exeDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            var script = ApplyUpdate.FindScript(exeDir, a.Package, File.Exists);
            ApplyResult result = null;
            if (script == null) Program.Problem(a, "apply: no installer\\apply.ps1 beside " + exeDir);
            else result = RunEngine(a, script);
            var id = ApplyUpdate.MessageOf(result);
            var handoff = ApplyUpdate.HandoffAllowed(result, ApplyUpdate.UpdateRoot());
            var icon = result != null && (result.Outcome == "applied" || result.Outcome == "up-to-date" || result.Outcome == "handoff") ? MessageBoxIcon.Information : MessageBoxIcon.Warning;
            MessageBox.Show(Strings.T(id + ".body"), Strings.T(id + ".title") + " - " + Program.ProductName, MessageBoxButtons.OK, icon);
            if (handoff)
            {
                try { using (Process.Start(new ProcessStartInfo(result.HandoffSetup) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(result.HandoffSetup) })) { } }
                catch (Exception e) { Program.Problem(a, "apply: the setup window of the new release did not start: " + e.Message); return 6; }
            }
            return ApplyUpdate.ExitCodeOf(result);
        }

        static ApplyResult RunEngine(SetupArgs a, string script)
        {
            var runDir = Path.Combine(a.RunRoot ?? Path.Combine(Path.GetTempPath(), "amdgpu-wddm-setup"), "apply-" + DateTime.UtcNow.ToString("yyyyMMddTHHmmssZ"));
            Directory.CreateDirectory(runDir);
            var resultFile = Path.Combine(runDir, "apply-result.json");
            ApplyResult result = null;
            using (var form = WorkingForm())
            {
                var worker = new Thread(() =>
                {
                    try
                    {
                        var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                            ApplyUpdate.EngineArguments(script, Path.GetFullPath(a.UpdateDir), resultFile))
                        { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
                        using (var p = Process.Start(psi))
                        {
                            if (!p.WaitForExit(ApplyUpdate.EngineBoundMinutes * 60 * 1000)) { Program.Problem(a, "apply: the engine did not end in " + ApplyUpdate.EngineBoundMinutes + " minutes"); try { p.Kill(); } catch (Exception) { } }
                        }
                        if (File.Exists(resultFile)) result = ApplyUpdate.ParseResult(File.ReadAllText(resultFile, Encoding.UTF8));
                        if (result != null && result.Outcome != "applied" && result.Outcome != "up-to-date") Program.Problem(a, "apply: " + result.Outcome + " " + result.Reason + ": " + result.Detail);
                    }
                    catch (Exception e) { Program.Problem(a, "apply: " + e); }
                    finally { try { form.BeginInvoke((Action)form.Close); } catch (Exception) { } }
                }) { IsBackground = true };
                form.Shown += (s, e) => worker.Start();
                Application.Run(form);
                worker.Join();
            }
            return result;
        }

        static Form WorkingForm()
        {
            var f = new Form
            {
                Text = Strings.T("apply.working.title") + " - " + Program.ProductName, FormBorderStyle = FormBorderStyle.FixedDialog,
                StartPosition = FormStartPosition.CenterScreen, MaximizeBox = false, MinimizeBox = false, ControlBox = false,
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(16), ShowInTaskbar = true,
            };
            var panel = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
            panel.Controls.Add(new Label { Text = Strings.T("apply.working.body"), AutoSize = true, MaximumSize = new Size(420, 0) });
            panel.Controls.Add(new ProgressBar { Style = ProgressBarStyle.Marquee, Width = 420, Height = 16, Margin = new Padding(0, 12, 0, 0) });
            f.Controls.Add(panel);
            return f;
        }
    }
}
