# Dry-run harness for killswitch.ps1 (owner's consent 2026-09-27 for the watched live-KD run).
# Two passes against a dummy process (hidden `ping -n 40 127.0.0.1`, no window, gone after ~40 s):
#   pass 1: -DryRun, GrowthLimitMB -1 (limit below the baseline => the trigger fires at once), expects "dry run, nothing killed"
#   pass 2: real kill with ProcessName 'ping' and the same impossible limit, expects the ping to disappear.
# kd.exe is never the target here. HAZARD: pass 2 kills every ping.exe on this PC, not only the dummy one, because
# the kill switch selects its victims by process name. Run it by hand on a quiet machine. It is therefore not part
# of tools/quality/quick.ps1; the offline gate there is selftest.ps1.
# Exit code 0 means both passes behaved as expected; 1 means one of them did not.
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
param(
    [string]$StateDir = (Join-Path (& { if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path } }) 'scratch\hostwatch')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $StateDir | Out-Null
$ks = Join-Path $PSScriptRoot 'killswitch.ps1'
$log = Join-Path $StateDir 'killswitch-dryrun.log'
$stop = Join-Path $StateDir 'killswitch-dryrun.stop'
function Run-Pass([string]$name, [string[]]$extra) {
    $ping = Start-Process -FilePath ping.exe -ArgumentList '-n', '40', '127.0.0.1' -WindowStyle Hidden -PassThru
    Add-Content $log ('{0} {1}: dummy ping pid {2}' -f (Get-Date).ToString('s'), $name, $ping.Id)
    if (Test-Path $stop) { Remove-Item $stop -Force }
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ks, '-GrowthLimitMB', '-1', '-ProcessName', 'ping', '-Log', $log, '-StopFile', $stop) + $extra
    $ksProc = Start-Process -FilePath powershell.exe -ArgumentList $args -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 4
    New-Item -ItemType File -Path $stop -Force | Out-Null
    $ksProc.WaitForExit(15000) | Out-Null
    Start-Sleep -Seconds 1
    $alive = -not $ping.HasExited
    Add-Content $log ('{0} {1}: killswitch exited={2} ping alive={3}' -f (Get-Date).ToString('s'), $name, $ksProc.HasExited, $alive)
    if ($alive) { Stop-Process -Id $ping.Id -Force -ErrorAction SilentlyContinue }
    return $alive
}
$a1 = Run-Pass 'pass1-dryrun' @('-DryRun')
$a2 = Run-Pass 'pass2-kill' @()
'pass1 (dry run) ping survived: {0}   expected True' -f $a1
'pass2 (real kill) ping survived: {0}   expected False' -f $a2
Get-Content $log | Select-Object -Last 12
if ($a1 -and -not $a2) { 'PASS'; exit 0 }
'FAIL: the kill switch did not behave as expected'
exit 1
