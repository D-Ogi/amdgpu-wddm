# The sign-in task of a tester PC: the scheduled task "amdgpu-wddm start window" (install.ps1 registers it from the
# plan of common.ps1: at the logon of any user, for the Users group, limited run level) runs this once per logon.
#
# The task does not run powershell.exe itself. It runs Nagi Control App as `amdgpu_wddm_control.exe --start-window
# -Seconds N`, a windows program, which starts this script through powershell.exe with no console window and gives
# back its exit code (StartWindowHost.cs). powershell.exe as the task's own program is a console program: on the lab
# its console was a Windows Terminal window that stayed on the screen (fact M899). Only an install without the
# control application (-NoControlApp) runs this script through powershell.exe directly.
#
# It does one thing in the session of the person who just signed in, and it needs no rights for it: it starts the
# sign-in notice of Nagi Control App (--start-notice). The control application reads the driver's start itself and
# shows a window ONLY when the driver is not running: a start the driver's own guard refused, with one button that
# turns the driver back on, or any other failed start, with the way to the application that explains it. On a
# healthy machine that process exits in a moment with nothing on the screen. The owner asked for this notice on
# 2026-10-13 ("Uzytkownik musi byc tego swiadomym od razu").
#
# Up to tester.31 this script also showed a small blue window with a countdown for two minutes, to keep DWM
# presenting while the start confirmation waited for a fresh flip. That window came from the CPU desktop and is gone
# (owner, 2026-10-22: "Niebieskie okienko jest z czasu pulpitu na CPU przeciez. Nie mozemy go usunac w calosci?").
# A desktop that DWM draws on the GPU flips whenever anything on it changes, and the confirmation of the same logon
# (start-confirm.ps1, the task "amdgpu-wddm start confirm", Local System) takes an idle one as it is: the KMD reports
# QUIET when every flip it was given has completed (driver/kmd/start_health.c). The task keeps its name, so an update
# replaces the task in place and needs no clean-up of an old one.
#
# BD-080: this half needs the session of the user who logged on and no rights at all. It therefore runs as that user,
# opens no handle to the driver, writes nothing and confirms nothing. Nothing here fails the logon.
#
# What it reports. This script writes no file (a standard user may write nowhere of ours), so its exit code is the
# whole report, and the scheduled task keeps it as LastTaskResult:
#   0  the notice was started, or there was no control application to start
#   2  Windows refused to start the control application
# The control application that starts this script adds two codes of its own: 4 it stopped this script at -Seconds
# plus 30 s, 5 it could not start this script (StartWindowHost.cs). -Seconds is still taken, because the host passes
# it; this script no longer waits for anything.
param([int]$Seconds = 120)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# The sign-in notice, started and never waited for: a person whose driver is not running should read that before
# anything else on the screen. A control application that is missing (an install with -NoControlApp) changes nothing.
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
        # never appears, which leaves the person with the silent basic display this notice is against.
        # The application is a windowed program with no console, so there is nothing a hidden style would hide.
        Start-Process -FilePath $exe -ArgumentList '--start-notice' | Out-Null
    }
} catch {
    exit 2
}
exit 0
