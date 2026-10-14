# How soon Nagi Control App and the single-file setup put their first window on the lab's screen, cold and warm, and
# whether a second start of the control app opens nothing and brings the first one forward (owner, 2026-10-14).
# Windows PowerShell 5.1, over ssh (elevated, session 0), bounded to three minutes:
#
#   python tools\win\target.py ps tools\win\app-launch\measure-launch.ps1 -ControlExe <exe> -SetupExe <exe>
#
#   -ControlExe   amdgpu_wddm_control.exe, in its folder of the build or the install
#   -SetupExe     amdgpu-wddm-setup-<version>.exe, the single file of a release
#   -Warm         warm starts per program after the cold one (default 3)
#   -MainHeight   the height in logical pixels from which a window counts as the program's main window (default
#                 420; both loading windows are lower, both main windows higher)
#
# Give one or both. Before and after a change are two commands, one per build, each inside its own three minutes.
#
# Why a scheduled task. Over ssh the script runs in session 0, where no window can appear. The programs therefore start
# in the console session, as the user who is logged on there, through one task per phase: the control app with the
# limited run level, as from the Start menu, and setup with the highest run level, as after a Yes in the prompt of
# Windows (the prompt itself is on the secure desktop, and nobody here can answer it; before the prompt, setup shows
# the same loading window as after it). In that session a small watcher (the C# below, compiled here by Add-Type,
# with no window of its own) starts the program and looks at its top-level windows once a millisecond:
#   first-ms   the first visible window of the process (the loading window of a build that has one)
#   main-ms    the first visible window at least -MainHeight high (the main window)
#   alone-ms   the main window is the only window of the process left (the loading window has closed)
# All three count from the moment before the watcher creates the process. The watcher then closes the main window
# (WM_CLOSE) and ends the process only when it does not exit in 10 s.
#
# Cold means that the standby list of the memory manager was purged just before the start, so the program's own files
# come from the disk. Files that a running process maps stay in memory (the .NET runtime under this PowerShell), and
# Windows Defender keeps what it learned of the setup file, so a cold run is not a cold boot.
#
# The second-start checks (control app): a second start while the first shows its main window minimised, and a
# second start while the first still shows its loading window. Each passes when the second process ends by itself
# with code 0 within 10 s and shows no window, the minimised window comes back, and the first reaches its main window.
#
# The script refuses to run while either program already runs (it never stops a process it did not start), and setup
# when the console user is not an administrator (Windows would ask). It stops only the processes its watcher
# started, and removes only the setup folders under %ProgramData%\amdgpu-wddm\setup that appeared during its run.
# It writes under -Work and registers one task, which it removes on every exit path.
#
# Exit codes: 0 every start reached its main window and every second-start check passed, 1 something failed or did
# not fit in the bound (a run that does not fit is skipped, never cut short), 2 refused before any start.
#
# The console user starts the programs, so that account must be able to read their folders: the install folder and
# a folder under C:\Users\Public are; a copy elsewhere may need read access first.
param(
    [string]$ControlExe = '',
    [string]$SetupExe = '',
    [int]$Warm = 3,
    [int]$MainHeight = 420,
    [int]$BoundSeconds = 170,
    [string]$Work = 'C:\BC250\tmp\app-launch',
    [string]$TaskName = 'BC250 app launch probe'
)
$ErrorActionPreference = 'Continue'
$clock = [Diagnostics.Stopwatch]::StartNew()
$bad = @()
$started = @()
$rows = @()
$setupBefore = $null

$source = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// launch-watch.exe purge  <out>
// launch-watch.exe runs   <out> <deadline-ms> <run-ms> <main-height> <count> <exe>
// launch-watch.exe second <out> <deadline-ms> <run-ms> <main-height> <main|loading> <exe>
// It is not aware of the screen's DPI, so Windows gives it every window's size in logical pixels.
public static class LaunchWatch
{
    delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, Pack = 1)] struct OnePrivilege { public int Count; public long Luid; public int Attributes; }

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr lparam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out Rect r);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, int access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref OnePrivilege state, int length, IntPtr previous, IntPtr returned);
    [DllImport("ntdll.dll")] static extern int NtSetSystemInformation(int infoClass, ref int info, int length);

    const uint WM_CLOSE = 0x0010;
    const int SW_MINIMIZE = 6, DWMWA_CLOAKED = 14;

    sealed class Shown { public IntPtr Handle; public int Width, Height; }

    static TextWriter _out;

    static void Say(string line) { _out.WriteLine(line); _out.Flush(); }

    // The visible top-level windows of one process, at least 32 x 32: the hidden helper windows of WinForms and the
    // input method do not count, nor does a cloaked one.
    static List<Shown> Windows(int pid)
    {
        var found = new List<Shown>();
        EnumWindows((h, l) =>
        {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner != (uint)pid || !IsWindowVisible(h)) return true;
            int cloaked;
            if (DwmGetWindowAttribute(h, DWMWA_CLOAKED, out cloaked, 4) == 0 && cloaked != 0) return true;
            Rect r;
            if (!GetWindowRect(h, out r)) return true;
            int w = r.Right - r.Left, ht = r.Bottom - r.Top;
            if (w >= 32 && ht >= 32) found.Add(new Shown { Handle = h, Width = w, Height = ht });
            return true;
        }, IntPtr.Zero);
        return found;
    }

    static Process Start(string exe)
    {
        // CreateProcess with no startup information about the window, as Explorer gives none that matters here.
        var info = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe) };
        var p = Process.Start(info);
        Say("pid=" + p.Id);
        return p;
    }

    // What one process showed until its main window stood alone, it ended, or the bound passed.
    sealed class Watch
    {
        public long First = -1, Main = -1, Alone = -1;
        public int Windows, FirstWidth, FirstHeight, MainWidth, MainHeight;
        public IntPtr MainHandle;
        readonly HashSet<IntPtr> _seen = new HashSet<IntPtr>();

        public void Look(Process p, Stopwatch clock, int mainHeight)
        {
            long now = clock.ElapsedMilliseconds;
            var list = LaunchWatch.Windows(p.Id);
            foreach (var w in list) if (_seen.Add(w.Handle)) Windows++;
            if (First < 0 && list.Count > 0) { First = now; FirstWidth = list[0].Width; FirstHeight = list[0].Height; }
            if (Main < 0)
                foreach (var w in list)
                    if (w.Height >= mainHeight) { Main = now; MainHandle = w.Handle; MainWidth = w.Width; MainHeight = w.Height; break; }
            if (Main >= 0 && Alone < 0 && list.Count == 1 && list[0].Handle == MainHandle) Alone = now;
        }

        public bool Done { get { return Alone >= 0; } }

        public string Text
        {
            get
            {
                return "first-ms=" + First + " main-ms=" + Main + " alone-ms=" + Alone + " windows=" + Windows +
                    " first-size=" + FirstWidth + "x" + FirstHeight + " main-size=" + MainWidth + "x" + MainHeight;
            }
        }
    }

    static void WatchUntil(Process p, Watch w, Stopwatch clock, long bound, int mainHeight, Func<Watch, bool> enough)
    {
        while (clock.ElapsedMilliseconds < bound)
        {
            w.Look(p, clock, mainHeight);
            if (enough(w) || p.HasExited) return;
            Thread.Sleep(1);
        }
    }

    // The main window gets WM_CLOSE (every window, when there is none); a process that is still there after 10 s is
    // ended. Only processes this watcher started come here.
    static string Close(Process p, Watch w)
    {
        if (p.HasExited) return "closed=exited exit=" + p.ExitCode;
        if (w != null && w.MainHandle != IntPtr.Zero) PostMessage(w.MainHandle, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        else foreach (var s in Windows(p.Id)) PostMessage(s.Handle, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        if (p.WaitForExit(10000)) return "closed=wm_close exit=" + p.ExitCode;
        try { p.Kill(); p.WaitForExit(5000); } catch (Exception) { }
        return "closed=killed";
    }

    static int Runs(Stopwatch total, long deadline, int runMs, int mainHeight, int count, string exe)
    {
        for (int i = 1; i <= count; i++)
        {
            if (total.ElapsedMilliseconds + runMs > deadline) { Say("run=" + i + " skipped=time"); continue; }
            var clock = Stopwatch.StartNew();
            using (var p = Start(exe))
            {
                var w = new Watch();
                WatchUntil(p, w, clock, runMs, mainHeight, x => x.Done);
                Say("run=" + i + " " + w.Text + " " + Close(p, w));
            }
            Thread.Sleep(1000);
        }
        return 0;
    }

    static int Second(Stopwatch total, long deadline, int runMs, int mainHeight, bool atLoading, string exe)
    {
        if (total.ElapsedMilliseconds + runMs + 15000 > deadline) { Say("second skipped=time"); return 0; }
        var clock = Stopwatch.StartNew();
        using (var first = Start(exe))
        {
            var w = new Watch();
            WatchUntil(first, w, clock, runMs, mainHeight, x => atLoading ? x.First >= 0 : x.Done);
            if (atLoading ? w.First < 0 : !w.Done)
            {
                Say("second first-not-ready " + w.Text + " " + Close(first, w));
                return 0;
            }
            if (!atLoading)
            {
                ShowWindow(w.MainHandle, SW_MINIMIZE);
                var wait = Stopwatch.StartNew();
                while (!IsIconic(w.MainHandle) && wait.ElapsedMilliseconds < 2000) Thread.Sleep(5);
            }
            bool minimised = !atLoading && IsIconic(w.MainHandle);

            var at = Stopwatch.StartNew();
            long exitMs = -1;
            int secondWindows = 0, secondExit = -1;
            var seen = new HashSet<IntPtr>();
            using (var second = Start(exe))
            {
                while (at.ElapsedMilliseconds < 10000)
                {
                    foreach (var s in Windows(second.Id)) if (seen.Add(s.Handle)) secondWindows++;
                    if (second.HasExited) { exitMs = at.ElapsedMilliseconds; secondExit = second.ExitCode; break; }
                    Thread.Sleep(1);
                }
                if (!second.HasExited) Say("second-stayed " + Close(second, null));
            }

            // The first start: back from the taskbar, or on to its main window.
            long restoredMs = -1;
            if (!atLoading)
            {
                while (at.ElapsedMilliseconds < 13000) { if (!IsIconic(w.MainHandle)) { restoredMs = at.ElapsedMilliseconds; break; } Thread.Sleep(1); }
            }
            else WatchUntil(first, w, clock, runMs, mainHeight, x => x.Done);
            uint front;
            GetWindowThreadProcessId(GetForegroundWindow(), out front);
            Say("second at=" + (atLoading ? "loading" : "main") + " exit-ms=" + exitMs + " exit=" + secondExit +
                " second-windows=" + secondWindows + " minimised=" + (minimised ? 1 : 0) + " restored-ms=" + restoredMs +
                " front=" + (front == (uint)first.Id ? 1 : 0) + " first-" + w.Text + " " + Close(first, w));
        }
        return 0;
    }

    // The standby list of the memory manager, emptied (SystemMemoryListInformation, MemoryPurgeStandbyList). Needs
    // SeProfileSingleProcessPrivilege, which an elevated administrator holds and enables here.
    static int Purge()
    {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) { Say("purge=no-token"); return 1; }
        try
        {
            var p = new OnePrivilege { Count = 1, Attributes = 2 };
            if (!LookupPrivilegeValue(null, "SeProfileSingleProcessPrivilege", out p.Luid) ||
                !AdjustTokenPrivileges(token, false, ref p, 0, IntPtr.Zero, IntPtr.Zero) || Marshal.GetLastWin32Error() != 0)
            { Say("purge=no-privilege"); return 1; }
        }
        finally { CloseHandle(token); }
        int command = 4;
        int status = NtSetSystemInformation(80, ref command, 4);
        Say("purge=" + (status == 0 ? "ok" : "0x" + status.ToString("x8")));
        return status == 0 ? 0 : 1;
    }

    public static int Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "purge")
            using (_out = new StreamWriter(args[1], false, new UTF8Encoding(false)))
                return Purge();
        if (args.Length != 7 || (args[0] != "runs" && args[0] != "second")) return 2;
        using (_out = new StreamWriter(args[1], true, new UTF8Encoding(false)))
        {
            var total = Stopwatch.StartNew();
            timeBeginPeriod(1);
            try
            {
                long deadline = long.Parse(args[2]);
                int runMs = int.Parse(args[3]), mainHeight = int.Parse(args[4]);
                if (args[0] == "runs") return Runs(total, deadline, runMs, mainHeight, int.Parse(args[5]), args[6]);
                return Second(total, deadline, runMs, mainHeight, args[5] == "loading", args[6]);
            }
            catch (Exception e) { Say("error=" + e.GetType().Name + ": " + e.Message.Replace("\r", " ").Replace("\n", " ")); return 1; }
            finally { timeEndPeriod(1); }
        }
    }
}
'@

function Remove-Probe {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
}

function Get-Left { return [int]($BoundSeconds - $clock.Elapsed.TotalSeconds) }

# One phase: the watcher in the console session, as a task, until it ends or the time is up. Returns its lines.
function Invoke-Phase([string]$Program, [string]$Phase, [string]$RunLevel, [string[]]$WatchArgs, [int]$RunMs) {
    $out = Join-Path $script:dir "$Program-$Phase.txt"
    $left = (Get-Left) - 8
    if ($left -lt [Math]::Ceiling($RunMs / 1000) + 5) { return @('skipped=time') }
    $argText = (@($WatchArgs[0], $out, ($left * 1000), $RunMs, $MainHeight) + @($WatchArgs[1..($WatchArgs.Count - 1)]) |
        ForEach-Object { '"' + $_ + '"' }) -join ' '
    Remove-Probe
    $principal = New-ScheduledTaskPrincipal -UserId $script:user -LogonType Interactive -RunLevel $RunLevel
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ([Math]::Max(60, $left + 5))) `
        -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $action = New-ScheduledTaskAction -Execute $script:watch -Argument $argText -WorkingDirectory $script:dir
    Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    $end = [DateTime]::UtcNow.AddSeconds($left)
    do {
        Start-Sleep -Milliseconds 500
        $state = (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue).State
    } while ($state -eq 'Running' -and [DateTime]::UtcNow -lt $end)
    if ($state -eq 'Running') {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        $script:bad += "$Program $Phase did not end inside the bound"
    }
    Remove-Probe
    $lines = if (Test-Path -LiteralPath $out) { @(Get-Content -LiteralPath $out) } else { @() }
    foreach ($l in $lines) { if ($l -match '^pid=(\d+)$') { $script:started += [int]$Matches[1] } }
    return $lines
}

function Get-Field([string]$Line, [string]$Key) {
    if ($Line -match "(^| )$([regex]::Escape($Key))=(\S+)") { return $Matches[2] } else { return $null }
}

# The runs of one phase as rows of the summary; a run without a main window, or one the watcher had to end, fails.
function Add-Runs([string]$Program, [string]$Phase, [string[]]$Lines) {
    if ($Lines -contains 'skipped=time') { $script:bad += "$Program ${Phase}: skipped, no time left"; return }
    foreach ($l in $Lines) {
        if ($l -match '^error=') { $script:bad += "$Program ${Phase}: watcher $l"; continue }
        if ($l -notmatch '^run=') { continue }
        $run = Get-Field $l 'run'
        if (Get-Field $l 'skipped') { $script:bad += "$Program $Phase run ${run}: skipped, no time left"; continue }
        $first = Get-Field $l 'first-ms'; $main = Get-Field $l 'main-ms'; $alone = Get-Field $l 'alone-ms'
        $script:rows += '{0,-8} {1,-5} run {2}  first {3,6} ms  main {4,6} ms  alone {5,6} ms  windows {6}  first {7}  main {8}  {9}' -f `
            $Program, $Phase, $run, $first, $main, $alone, (Get-Field $l 'windows'), (Get-Field $l 'first-size'),
            (Get-Field $l 'main-size'), (Get-Field $l 'closed')
        if ([int]$main -lt 0) { $script:bad += "$Program $Phase run ${run}: no main window" }
        elseif ([int]$alone -lt 0) { $script:bad += "$Program $Phase run ${run}: a second window stayed beside the main window" }
        if ((Get-Field $l 'closed') -eq 'killed') { $script:bad += "$Program $Phase run ${run}: did not close on WM_CLOSE" }
    }
    if (-not @($Lines | Where-Object { $_ -match '^run=' }).Count) { $script:bad += "$Program ${Phase}: no run recorded" }
}

function Add-Second([string]$Phase, [string[]]$Lines) {
    if ($Lines -contains 'skipped=time') { $script:bad += "control ${Phase}: skipped, no time left"; return }
    $l = @($Lines | Where-Object { $_ -match '^second ' } | Select-Object -Last 1)
    if (-not $l.Count) { $script:bad += "control ${Phase}: no result"; return }
    $l = $l[0]
    if ($l -match 'skipped=time') { $script:bad += "control ${Phase}: skipped, no time left"; return }
    if ($l -match 'first-not-ready') { $script:bad += "control ${Phase}: the first start did not get there ($l)"; return }
    $script:rows += "control  $Phase  $($l -replace '^second ', '')"
    if (@($Lines | Where-Object { $_ -match '^second-stayed' }).Count) { $script:bad += "control ${Phase}: the second start stayed open" }
    if ((Get-Field $l 'exit') -ne '0') { $script:bad += "control ${Phase}: the second start did not end with 0" }
    if ((Get-Field $l 'second-windows') -ne '0') { $script:bad += "control ${Phase}: the second start showed a window" }
    if ($Phase -eq 'second-main') {
        if ((Get-Field $l 'minimised') -ne '1') { $script:bad += 'control second-main: the first window could not be minimised' }
        elseif ([int](Get-Field $l 'restored-ms') -lt 0) { $script:bad += 'control second-main: the minimised window did not come back' }
    }
    elseif ([int](Get-Field $l 'first-alone-ms') -lt 0) { $script:bad += 'control second-loading: the first start did not reach its main window' }
}

try {
    "utc $([DateTime]::UtcNow.ToString('o'))"
    $programs = @()
    if ($ControlExe) { $programs += @{ Name = 'control'; Exe = $ControlExe; Level = 'Limited'; RunMs = 30000 } }
    if ($SetupExe) { $programs += @{ Name = 'setup'; Exe = $SetupExe; Level = 'Highest'; RunMs = 45000 } }
    if (-not $programs.Count) { throw 'refused: give -ControlExe, -SetupExe or both' }
    if ($BoundSeconds -gt 175) { throw 'refused: the bound of a lab trial is three minutes' }

    $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $explorer) { throw 'refused: no explorer.exe, so nobody is logged on at the console' }
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner
    $user = '{0}\{1}' -f $owner.Domain, $owner.User
    "console user $user session $($explorer.SessionId)"

    foreach ($p in $programs) {
        if (-not (Test-Path -LiteralPath $p.Exe -PathType Leaf)) { throw "refused: no file $($p.Exe)" }
        $p.Exe = (Resolve-Path -LiteralPath $p.Exe).ProviderPath
        $name = [IO.Path]::GetFileNameWithoutExtension($p.Exe)
        $running = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        if ($p.Name -eq 'control') { $running += @(Get-Process -Name 'amdgpu_wddm_control' -ErrorAction SilentlyContinue) }
        if ($running.Count) { throw "refused: $name already runs (pid $(($running | ForEach-Object { $_.Id } | Sort-Object -Unique) -join ', ')); close it first, this script stops no process it did not start" }
        $item = Get-Item -LiteralPath $p.Exe
        "$($p.Name) $($p.Exe) $($item.Length) bytes sha256 $((Get-FileHash -LiteralPath $p.Exe -Algorithm SHA256).Hash) version $($item.VersionInfo.FileVersion)"
    }
    if ($SetupExe) {
        # By SID: the name of the group is translated on a Windows in another language.
        $admins = @(Get-LocalGroupMember -SID 'S-1-5-32-544' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        if ($admins -notcontains $user) { throw "refused: $user is not an administrator, so setup would wait for the prompt of Windows" }
    }

    $dir = Join-Path $Work ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
    [void][IO.Directory]::CreateDirectory($dir)
    # The console user's watcher writes its lines here, also with the limited run level.
    & icacls.exe $dir /grant "${user}:(OI)(CI)M" | Out-Null
    $watch = Join-Path $dir 'launch-watch.exe'
    Add-Type -TypeDefinition $source -OutputAssembly $watch -OutputType WindowsApplication
    if (-not (Test-Path -LiteralPath $watch)) { throw 'the watcher did not compile' }
    "work $dir"

    $setupRoot = Join-Path $env:ProgramData 'amdgpu-wddm\setup'
    if ($SetupExe) {
        $setupBefore = @(if (Test-Path -LiteralPath $setupRoot) { Get-ChildItem -LiteralPath $setupRoot -Force -ErrorAction Stop | ForEach-Object { $_.Name } })
    }

    foreach ($p in $programs) {
        $purgeOut = Join-Path $dir "$($p.Name)-purge.txt"
        $pp = Start-Process -FilePath $watch -ArgumentList @('purge', ('"' + $purgeOut + '"')) -Wait -PassThru
        $purge = if (Test-Path -LiteralPath $purgeOut) { ((Get-Content -LiteralPath $purgeOut) -join ' ').Trim() } else { "no output, exit $($pp.ExitCode)" }
        "$($p.Name) cold: $purge"
        if ($purge -ne 'purge=ok') { $bad += "$($p.Name) cold: the standby list was not purged ($purge)" }
        Add-Runs $p.Name 'cold' (Invoke-Phase $p.Name 'cold' $p.Level @('runs', '1', $p.Exe) $p.RunMs)
        Add-Runs $p.Name 'warm' (Invoke-Phase $p.Name 'warm' $p.Level @('runs', "$Warm", $p.Exe) $p.RunMs)
        if ($p.Name -eq 'control') {
            foreach ($at in @('main', 'loading')) {
                Add-Second "second-$at" (Invoke-Phase $p.Name "second-$at" $p.Level @('second', $at, $p.Exe) $p.RunMs)
            }
        }
    }
}
catch { $bad += "$($_.Exception.Message)" }
finally {
    Remove-Probe
    # Only what this run started: the processes its watcher recorded, if one is still there.
    $exes = @($programs | ForEach-Object { $_.Exe })
    foreach ($id in ($started | Sort-Object -Unique)) {
        $left = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($left -and $exes -contains $left.Path) {
            $bad += "process $id ($($left.ProcessName)) was still running at the end, stopped"
            Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
        }
    }
    # Only the setup folders that appeared during this run (a run that had to be ended leaves its unpacked files).
    if ($null -ne $setupBefore -and (Test-Path -LiteralPath $setupRoot)) {
        foreach ($d in @(Get-ChildItem -LiteralPath $setupRoot -Force | Where-Object { $setupBefore -notcontains $_.Name })) {
            Remove-Item -LiteralPath $d.FullName -Recurse -Force -ErrorAction SilentlyContinue
            "removed the setup folder $($d.Name), which this run left"
        }
    }
}

''
foreach ($r in $rows) { $r }
"elapsed $([int]$clock.Elapsed.TotalSeconds) s"
if ($bad.Count) {
    foreach ($b in $bad) { "problem: $b" }
    if (@($bad | Where-Object { $_ -like 'refused:*' }).Count) { 'app launch: REFUSED'; exit 2 }
    "app launch: FAIL, $($bad.Count) problem(s)"
    exit 1
}
'app launch: ok'
exit 0
