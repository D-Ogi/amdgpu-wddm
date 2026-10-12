# The logon window of a tester PC: the scheduled task "amdgpu-wddm start window" (install.ps1 registers it from the
# plan of common.ps1: at the logon of any user, for the Users group, limited run level) runs this once per logon.
#
# What it is for. The KMD confirms a start only when the start has been ready for 60 s and a presentation completed
# within the last 15 s (BC250_START_HEALTH_MIN_MS and BC250_START_HEALTH_FRESH_MS in driver\kmd\bc250kmd_escape.h).
# An idle desktop presents too rarely for that, so this script shows a small top-most window whose text changes
# every 250 ms: DWM keeps presenting while it is open, and the confirmation of the same logon (start-confirm.ps1,
# the task "amdgpu-wddm start confirm", which runs as Local System) reaches the milestone.
#
# BD-080: this is the half of the work that needs the session of the user who logged on, and it is also the half
# that needs no rights at all. It therefore runs as that user, whoever they are, with the rights they have. It
# opens no handle to the driver, reads and writes no registry value and confirms nothing: a standard user's CONFIRM
# would be refused by the kernel anyway (start_health.c). Nothing here fails the logon: a window that cannot open
# leaves the confirmation to its own fallback, which keeps the machine off Microsoft Basic Display.
#
# It closes itself after -Seconds (120 by default), and nothing stays resident. The task's own time limit is the
# second bound, and a logoff takes the window with the session. The window stays up for the whole of -Seconds: this
# task knows nothing of the confirmation's progress, where the single task of the older releases closed the window
# as soon as its own poll ended the run (INSTALL.md and the release note say so).
#
# What it reports. This script writes no file (a standard user may write nowhere of ours), so its exit code is the
# whole report, and the scheduled task keeps it as LastTaskResult:
#   0  the window was shown and closed itself, or -Seconds was below one
#   2  the window could not be shown (no WinForms assembly, no window station, a host that is not STA)
# The confirmation of the same logon runs as Local System, reads that result and writes it into its own log, so a
# window half that never ran is visible in C:\ProgramData\amdgpu-wddm\start-confirm.log. A failure here still fails
# no logon: the confirmation's own fallback keeps the machine off Microsoft Basic Display.
param([int]$Seconds = 120)
$ErrorActionPreference = 'Stop'
if ($Seconds -lt 1) { exit 0 }
try {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $clock = [Diagnostics.Stopwatch]::StartNew()
    # The same small window as the windowed run of start-confirm.ps1, in the same place: a tester who has seen one
    # has seen the other (docs\testing\INSTALL.md, "The small blue window after logon").
    $form = New-Object Windows.Forms.Form
    $form.FormBorderStyle = 'None'; $form.TopMost = $true; $form.ShowInTaskbar = $false; $form.ShowIcon = $false
    $form.StartPosition = 'Manual'; $form.Size = New-Object Drawing.Size(250, 22); $form.Location = New-Object Drawing.Point(8, 34)
    $form.BackColor = [Drawing.Color]::Navy
    $label = New-Object Windows.Forms.Label
    $label.Dock = 'Fill'; $label.ForeColor = [Drawing.Color]::Cyan; $label.Font = New-Object Drawing.Font('Consolas', 9)
    $form.Controls.Add($label)
    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 250
    $timer.Add_Tick({
        # The changing text is the point: it is what makes DWM present this window's frames.
        $label.Text = 'amdgpu-wddm start check ' + [DateTime]::Now.ToString('HH:mm:ss.f')
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
