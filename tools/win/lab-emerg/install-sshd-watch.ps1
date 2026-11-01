param([switch]$Uninstall)
# LAB (elevated, through target.py ps): installs or removes the BD-051 sshd watch (sshd-watch.ps1) on unit A.
# Install: push sshd-watch.ps1 and scripts\sshd-stall-forensics.ps1 to C:\BC250\tmp first. This script copies them
# to C:\BC250\emergency and C:\BC250\emergency\scripts, registers the task "Lab sshd watch" (SYSTEM, at startup,
# started again after a failure, no time limit), starts it and prints the first probe. The task name stays outside
# the pattern 'BC250|DWM|G0|WSI' of the competing-task gate of the trial kits, as the emergency channel's name does.
# Uninstall: the stop file first, so that the watch ends between two probes, then the task. Files and logs stay.
$ErrorActionPreference = 'Stop'
$base = 'C:\BC250\emergency'
$task = 'Lab sshd watch'
$watchDir = Join-Path $base 'sshd-watch'
$stopFile = Join-Path $watchDir 'stop'
function Get-WatchProcess {
    @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.CommandLine -match 'emergency\\sshd-watch\.ps1' })
}
function Stop-Watch {
    if (-not (Get-WatchProcess).Count) { return }
    $null = New-Item -ItemType Directory -Force -Path $watchDir
    Set-Content -LiteralPath $stopFile -Value 'stop' -Encoding ascii
    # The watch reads the stop file every second while it waits. A probe takes at most its timeout (10 s).
    $until = (Get-Date).AddSeconds(20)
    while ((Get-WatchProcess).Count -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
    # Unregister-ScheduledTask does not stop a running process, so a watch that is still there (in a heal) is ended.
    foreach ($w in Get-WatchProcess) { Stop-Process -Id $w.ProcessId -Force }
}
if ($Uninstall) {
    Stop-Watch
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    'uninstalled (files and logs kept in {0}; the stop file stays until the next install)' -f $watchDir
    return
}
foreach ($f in 'sshd-watch.ps1', 'sshd-stall-forensics.ps1') {
    if (-not (Test-Path -LiteralPath (Join-Path 'C:\BC250\tmp' $f))) { throw "push $f to C:\BC250\tmp first" }
}
$null = New-Item -ItemType Directory -Force -Path (Join-Path $base 'scripts'), $watchDir
Stop-Watch
Copy-Item 'C:\BC250\tmp\sshd-watch.ps1' (Join-Path $base 'sshd-watch.ps1') -Force
Copy-Item 'C:\BC250\tmp\sshd-stall-forensics.ps1' (Join-Path $base 'scripts\sshd-stall-forensics.ps1') -Force
Remove-Item -LiteralPath $stopFile -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $base 'sshd-watch.ps1') + '"')
$t = New-ScheduledTaskTrigger -AtStartup
$p = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $task -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 8
'task {0}, watch processes {1}' -f (Get-ScheduledTask -TaskName $task).State, (Get-WatchProcess).Count
foreach ($f in (Join-Path $base 'sshd-watch.ps1'), (Join-Path $base 'scripts\sshd-stall-forensics.ps1')) {
    '{0} {1}' -f (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash, $f
}
Get-Content (Join-Path $watchDir 'state.json') -ErrorAction SilentlyContinue
Get-Content (Join-Path $watchDir 'watch.log') -Tail 3 -ErrorAction SilentlyContinue
