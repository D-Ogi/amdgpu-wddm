// The host of the logon window task (the console of the hidden PowerShell came up at sign-in).
// The scheduled task "amdgpu-wddm start window" runs `amdgpu_wddm_control.exe --start-window -Seconds N` in the
// session of the person who signed in, and this file starts the release's start-window.ps1 from there.
//
// Why not powershell.exe in the task itself. powershell.exe is a console program, so Windows gives it a console when
// it creates the process, and -WindowStyle Hidden hides that console only after PowerShell has started. Where Windows
// Terminal is the default terminal the console is a Terminal window, and on the lab that window was still on the
// screen when the start window closed (fact M899, evidence/windows/2026-10-14-start-window-flash). This application
// is a windows-subsystem program and gets no console. It creates powershell.exe with CREATE_NO_WINDOW (documented for
// CreateProcess: the console process runs without a console window), which .NET sets for UseShellExecute false and
// CreateNoWindow true. So no console window exists at any time, and there is nothing for a default terminal to open.
//
// What it runs, and nothing else: powershell.exe of System32, -NoProfile -ExecutionPolicy Bypass -File, and the
// start-window.ps1 of this install (tools\ next to the control\ folder of this program), with the one parameter
// -Seconds. The path is not an argument, so this mode is not a general way to run a script.
//
// The exit code is the script's, so the task's LastTaskResult (which start-confirm.ps1 writes into its log) means what
// it meant when the task ran the script directly: 0 shown and closed, 2 could not be shown, 3 turned off by the
// person, 1 the script failed. This host adds two of its own:
//   4  the script did not end within -Seconds plus BoundSlackSeconds; this host stopped it (the process it started)
//   5  the script or powershell.exe is not there, the arguments are wrong, or Windows refused to start the process
// The wait stays inside the task's own time limit (3 minutes, common.ps1 Get-LogonTaskPlan) for -Seconds 120.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;

namespace AmdgpuWddmControl
{
    public static class StartWindowHost
    {
        public const string Verb = "--start-window";
        public const int ExitStopped = 4, ExitNotStarted = 5;
        public const int DefaultSeconds = 120, MaxSeconds = 3600, BoundSlackSeconds = 30;

        // The script of this install: <root>\tools\start-window.ps1, where <root> is the parent of this program's folder.
        public static string ScriptPath(string appDirectory)
        {
            string root = Path.GetDirectoryName(Path.GetFullPath(appDirectory).TrimEnd('\\'));
            return Path.Combine(root ?? appDirectory, "tools", "start-window.ps1");
        }

        public static string PowerShellPath()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe");
        }

        // The seconds of `--start-window [-Seconds N]`, or null for anything else.
        public static int? ParseSeconds(string[] args)
        {
            if (args == null || args.Length == 0 || args[0] != Verb) return null;
            if (args.Length == 1) return DefaultSeconds;
            int s;
            if (args.Length == 3 && string.Equals(args[1], "-Seconds", StringComparison.OrdinalIgnoreCase) &&
                int.TryParse(args[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out s) && s >= 0 && s <= MaxSeconds)
                return s;
            return null;
        }

        // The process this mode creates. Pure: no file is read and nothing starts.
        public static ProcessStartInfo Plan(string powershell, string script, int seconds)
        {
            return new ProcessStartInfo
            {
                FileName = powershell,
                Arguments = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Seconds " + seconds.ToString(CultureInfo.InvariantCulture),
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(script),
            };
        }

        public static int Run(string[] args, string appDirectory)
        {
            int? seconds = ParseSeconds(args);
            if (seconds == null) return ExitNotStarted;
            string script = ScriptPath(appDirectory), powershell = PowerShellPath();
            if (!File.Exists(script) || !File.Exists(powershell)) return ExitNotStarted;
            return Run(Plan(powershell, script, seconds.Value), (seconds.Value + BoundSlackSeconds) * 1000);
        }

        // Starts the plan, waits up to boundMs, and returns the child's exit code. A child still running at the bound
        // is stopped (this host started it) and the result is ExitStopped.
        public static int Run(ProcessStartInfo plan, int boundMs)
        {
            Process p;
            try { p = Process.Start(plan); }
            catch (Exception) { return ExitNotStarted; }
            if (p == null) return ExitNotStarted;
            using (p)
            {
                if (p.WaitForExit(boundMs)) return p.ExitCode;
                try { p.Kill(); p.WaitForExit(5000); } catch (Exception) { }
                return ExitStopped;
            }
        }
    }
}
