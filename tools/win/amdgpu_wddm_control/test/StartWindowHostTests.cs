// Host tests of the logon window task's host (src/StartWindowHost.cs): the arguments it takes, the process it plans,
// and three real runs of powershell.exe with a stand-in script in this test's temporary folder (no window: the runs
// use the plan's CREATE_NO_WINDOW, and the first one checks exactly that).
using System;
using System.Diagnostics;
using System.IO;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void StartWindowHostTests()
    {
        // ---- the arguments ---------------------------------------------------------------------------------------
        Equal((int?)120, StartWindowHost.ParseSeconds(new[] { "--start-window" }), "start window: no -Seconds is 120");
        Equal((int?)90, StartWindowHost.ParseSeconds(new[] { "--start-window", "-Seconds", "90" }), "start window: -Seconds 90");
        Equal((int?)0, StartWindowHost.ParseSeconds(new[] { "--start-window", "-seconds", "0" }), "start window: -seconds 0, any case");
        foreach (var bad in new[] {
            new[] { "--start-window", "-Seconds" }, new[] { "--start-window", "-Seconds", "x" }, new[] { "--start-window", "-Seconds", "-1" },
            new[] { "--start-window", "-Seconds", "3601" }, new[] { "--start-window", "90" }, new[] { "--start-window", "-Seconds", "9", "-File" },
            new[] { "--start-notice" }, new string[0] })
            Equal((int?)null, StartWindowHost.ParseSeconds(bad), "start window: refused: " + string.Join(" ", bad));

        // ---- the script of this install, and the process -----------------------------------------------------------
        Equal(@"C:\Program Files\amdgpu-wddm\tools\start-window.ps1", StartWindowHost.ScriptPath(@"C:\Program Files\amdgpu-wddm\control"),
            "start window: the script is tools\\start-window.ps1 next to control\\");
        Equal(@"C:\Program Files\amdgpu-wddm\tools\start-window.ps1", StartWindowHost.ScriptPath(@"C:\Program Files\amdgpu-wddm\control\"),
            "start window: the same with a trailing backslash");
        string ps = StartWindowHost.PowerShellPath();
        Check(ps.EndsWith(@"\System32\WindowsPowerShell\v1.0\powershell.exe", StringComparison.OrdinalIgnoreCase) && File.Exists(ps),
            "start window: powershell.exe of System32 (" + ps + ")");
        var plan = StartWindowHost.Plan(ps, @"C:\Program Files\amdgpu-wddm\tools\start-window.ps1", 120);
        Check(plan.CreateNoWindow && !plan.UseShellExecute, "start window: the plan asks for CREATE_NO_WINDOW (UseShellExecute false, CreateNoWindow true)");
        Equal("-NoProfile -ExecutionPolicy Bypass -File \"C:\\Program Files\\amdgpu-wddm\\tools\\start-window.ps1\" -Seconds 120", plan.Arguments,
            "start window: the command line, with no -WindowStyle");
        Check(!plan.RedirectStandardOutput && !plan.RedirectStandardError && !plan.RedirectStandardInput, "start window: no redirection");

        // ---- three runs with a stand-in script ----------------------------------------------------------------------
        string dir = Path.Combine(Path.GetTempPath(), "start-window-host-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            // 1. The child has no console window: GetConsoleWindow is NULL in it. Its exit code passes through.
            string noConsole = Path.Combine(dir, "no-console.ps1");
            File.WriteAllText(noConsole,
                "Add-Type -Namespace StartWindowHostTest -Name Native -MemberDefinition '[System.Runtime.InteropServices.DllImport(\"kernel32.dll\")] public static extern System.IntPtr GetConsoleWindow();'\r\n" +
                "if ([StartWindowHostTest.Native]::GetConsoleWindow() -eq [System.IntPtr]::Zero) { exit 3 } else { exit 7 }\r\n");
            var clock = Stopwatch.StartNew();
            Equal(3, StartWindowHost.Run(StartWindowHost.Plan(ps, noConsole, 1), 60000),
                "start window: the script runs with no console window (exit 3 passed through; 7 would be a console window)");
            // 2. A script that does not end is stopped at the bound, and the host says so.
            string slow = Path.Combine(dir, "slow.ps1");
            File.WriteAllText(slow, "Start-Sleep -Seconds 60\r\nexit 0\r\n");
            clock.Restart();
            Equal(StartWindowHost.ExitStopped, StartWindowHost.Run(StartWindowHost.Plan(ps, slow, 1), 3000), "start window: a script past the bound is stopped (exit 4)");
            Check(clock.Elapsed.TotalSeconds < 15, "start window: the stop came at the bound (" + clock.Elapsed.TotalSeconds.ToString("F1") + " s)");
            // 3. A program that is not there.
            Equal(StartWindowHost.ExitNotStarted, StartWindowHost.Run(StartWindowHost.Plan(Path.Combine(dir, "missing.exe"), noConsole, 1), 3000),
                "start window: a program that is not there (exit 5)");
            Equal(StartWindowHost.ExitNotStarted, StartWindowHost.Run(new[] { "--start-window", "-Seconds", "x" }, dir), "start window: wrong arguments (exit 5)");
            Equal(StartWindowHost.ExitNotStarted, StartWindowHost.Run(new[] { "--start-window" }, Path.Combine(dir, "control")),
                "start window: no tools\\start-window.ps1 next to the program (exit 5)");
        }
        finally { try { Directory.Delete(dir, true); } catch (Exception) { } }
    }
}
