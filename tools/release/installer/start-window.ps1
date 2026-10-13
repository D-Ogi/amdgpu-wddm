# The logon window of a tester PC: the scheduled task "amdgpu-wddm start window" (install.ps1 registers it from the
# plan of common.ps1: at the logon of any user, for the Users group, limited run level) runs this once per logon.
#
# It does two things in the session of the person who just signed in, and neither of them needs any rights:
#   1. It starts the sign-in notice of Nagi Control App (--start-notice). The control application reads the
#      driver's start itself and shows a window ONLY when the driver is not running: a start the driver's own guard
#      refused, with one button that turns the driver back on, or any other failed start, with the way to the
#      application that explains it. On a healthy machine that process exits in a moment with nothing on the screen.
#      This is the half the owner asked for on 2026-10-13 ("Uzytkownik musi byc tego swiadomym od razu"): up to this
#      release a person whose driver was switched off saw no word about it anywhere, only a slow desktop and games
#      that would not start.
#   2. It shows the small blue window (what it is for, and a countdown to its own close), unless the person turned
#      that off in the application.
#
# What the clock window is for. The KMD confirms a start only when the start has been ready for 60 s and a
# presentation completed within the last 15 s (BC250_START_HEALTH_MIN_MS and BC250_START_HEALTH_FRESH_MS in
# driver\kmd\bc250kmd_escape.h). An idle desktop presents too rarely for that, so this script shows a small top-most
# window whose text changes every 250 ms: DWM keeps presenting while it is open, and the confirmation of the same
# logon (start-confirm.ps1, the task "amdgpu-wddm start confirm", which runs as Local System) reaches the milestone.
#
# BD-080: this is the half of the work that needs the session of the user who logged on, and it is also the half
# that needs no rights at all. It therefore runs as that user, whoever they are, with the rights they have. It
# opens no handle to the driver, writes nothing at all, and confirms nothing: a standard user's CONFIRM would be
# refused by the kernel anyway (start_health.c). The one thing it reads is this user's own preference under
# HKCU (below), which is the user's to read and to change without an administrator. Nothing here fails the logon:
# a window that cannot open leaves the confirmation to its own fallback, which keeps the machine off Microsoft
# Basic Display.
#
# The switch. "Show the start check window after sign-in" in Nagi Control App (Settings), stored per user as
# HKCU\Software\amdgpu-wddm\Control ShowStartWindow: absent or any value but 0 means show it, 0 means do not. The
# application states the cost of turning it off next to the box: without the window the confirmation can stay
# unfinished, and the next start then runs at the fixed floor clock. The switch never touches the notice of point 1:
# a driver that was switched off is not something a person can ask us not to mention.
#
# It closes itself after -Seconds (120 by default), and nothing of this script stays resident. The task's own time
# limit is the second bound, and a logoff takes the window with the session. The window stays up for the whole of
# -Seconds: this task knows nothing of the confirmation's progress, where the single task of the older releases
# closed the window as soon as its own poll ended the run (INSTALL.md and the release note say so).
# The notice of point 1 is a process of its own and this script never waits for it. The run therefore always ends
# well inside the task's own time limit, the task completes normally, and a task that completes normally does not
# stop the processes it started: the notice stays on the screen until the person closes it.
#
# What it reports. This script writes no file (a standard user may write nowhere of ours), so its exit code is the
# whole report, and the scheduled task keeps it as LastTaskResult:
#   0  the window was shown and closed itself, or -Seconds was below one
#   2  the window could not be shown (no WinForms assembly, no window station, a host that is not STA)
#   3  the person turned the window off in the application; nothing was shown and nothing is wrong
# The confirmation of the same logon runs as Local System, reads that result and writes it into its own log, so a
# window half that never ran is visible in C:\ProgramData\amdgpu-wddm\start-confirm.log. A failure here still fails
# no logon: the confirmation's own fallback keeps the machine off Microsoft Basic Display.
param([int]$Seconds = 120)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# The sign-in notice, started first and never waited for: a person whose driver is not running should read that
# before anything else on the screen. Everything here is guarded, because this is an addition to a task whose
# first duty is the clock window below, and a control application that is missing (an install with
# -NoControlApp) or a start that Windows refuses must change nothing about that duty.
try {
    # The install root is the parent of tools\, and the application sits in control\ under it, named by the
    # release manifest (install.ps1 takes the same name from manifest.json control_app_exe).
    $root = Split-Path $here
    $exeName = 'amdgpu_wddm_control.exe'
    try {
        $named = [string](Get-Content -LiteralPath (Join-Path $root 'manifest.json') -Raw | ConvertFrom-Json).control_app_exe
        if ($named -and ($named -notmatch '[\\/]')) { $exeName = $named }
    } catch { }
    $exe = Join-Path $root (Join-Path 'control' $exeName)
    if (Test-Path -LiteralPath $exe -PathType Leaf) {
        # No -WindowStyle here, and no other startup information: Windows ignores the first ShowWindow of a
        # process whose launcher supplied STARTUPINFO, and that first show is the only one a WinForms window
        # makes (Application.Run -> Form.Visible). Started with -WindowStyle Hidden the notice is created and
        # never appears, which leaves the person with the silent basic display this whole window is against.
        # The application is a windowed program with no console, so there is nothing a hidden style would hide.
        Start-Process -FilePath $exe -ArgumentList '--start-notice' | Out-Null
    }
} catch { }

# This user's own preference for the clock window. Read only, and absent counts as on, so a fresh install and a
# profile that has never opened the application both show the window.
$showWindow = $true
try {
    $pref = Get-ItemProperty -Path 'HKCU:\Software\amdgpu-wddm\Control' -Name 'ShowStartWindow' -ErrorAction Stop
    $value = $pref.ShowStartWindow
    if (($value -is [int]) -and ($value -eq 0)) { $showWindow = $false }
} catch { }
if (-not $showWindow) { exit 3 }

if ($Seconds -lt 1) { exit 0 }

# The window says what it is for and when it goes (owner, 2026-10-13: a tester saw a blue box with a clock and no
# word about it). The countdown is also the changing text that keeps DWM presenting: it changes once a second, well
# inside the 15 s freshness of the KMD's start health. English only, like the rest of this script's output.
function Format-StartWindowText([double]$Left) {
    $s = [int][Math]::Max(0, [Math]::Ceiling($Left))
    return ("Nagi start check: confirming that the GPU driver started correctly.`r`n" +
        'Nothing to do. This window closes by itself in {0}:{1:00}.') -f [int][Math]::Floor($s / 60), ($s % 60)
}
try {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $clock = [Diagnostics.Stopwatch]::StartNew()
    # The same small window as the windowed run of start-confirm.ps1, in the same place: a tester who has seen one
    # has seen the other (docs\testing\INSTALL.md, "The small blue window after logon").
    $form = New-Object Windows.Forms.Form
    $form.FormBorderStyle = 'None'; $form.TopMost = $true; $form.ShowInTaskbar = $false; $form.ShowIcon = $false
    $form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(8, 34)
    $form.BackColor = [Drawing.Color]::Navy
    $label = New-Object Windows.Forms.Label
    $label.Dock = 'Fill'; $label.ForeColor = [Drawing.Color]::Cyan; $label.Font = New-Object Drawing.Font('Consolas', 9)
    $label.Padding = New-Object Windows.Forms.Padding(4, 3, 4, 3)
    $label.Text = Format-StartWindowText $Seconds
    # Sized to its two lines at the longest countdown, so no text is cut at any scale or font size.
    $need = [Windows.Forms.TextRenderer]::MeasureText($label.Text, $label.Font)
    $form.ClientSize = New-Object Drawing.Size(($need.Width + 12), ($need.Height + 8))
    $form.Controls.Add($label)
    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 250
    $timer.Add_Tick({
        # The changing text is the point: it is what makes DWM present this window's frames.
        $label.Text = Format-StartWindowText ($Seconds - $clock.Elapsed.TotalSeconds)
        if ($clock.Elapsed.TotalSeconds -ge $Seconds) { $timer.Stop(); $form.Close() }
    })
    $timer.Start()
    [void]$form.ShowDialog()
    $timer.Dispose(); $form.Dispose()
} catch {
    # Exit 2, not 0: a window that cannot open is the one thing this task has to report, and its result is the only
    # place it can say so. The confirmation's log carries this number (see the header).
    exit 2
}
exit 0
