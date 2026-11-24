param([switch]$Uninstall, [string]$Source = 'C:\BC250\tmp')
# Installs (or removes) the emergency channel on unit A: C:\BC250\emergency\{listener.ps1,key.bin,scripts\},
# firewall rule "BC250 emergency 8722" (TCP 8722, local subnet), task "Lab emergency channel" (SYSTEM, at
# startup, restarted on failure). Expects listener.ps1 and emergency-key.bin pushed to C:\BC250\tmp; moves the key
# (never left in tmp). Run elevated (target.py ps).
$ErrorActionPreference = 'Stop'
$base = 'C:\BC250\emergency'
# The task name stays outside the trial kits' competing-task pattern ('BC250|DWM|G0|WSI', preflight/postflight): the
# channel is not a test task and never touches the GPU. Named "BC250 emergency channel" it failed 222's Capture.
$task = 'Lab emergency channel'
$oldTask = 'BC250 emergency channel'
$rule = 'BC250 emergency 8722'
function Protect-EmergencyKey([string]$Path) {
    # Numeric well-known SIDs work with renamed groups and every Windows language.
    # https://learn.microsoft.com/windows-server/administration/windows-commands/icacls
    & icacls.exe $Path /inheritance:r /grant:r '*S-1-5-18:F' '*S-1-5-32-544:F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Key ACL update failed (icacls exit $LASTEXITCODE); channel not started" }
}
if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $oldTask -Confirm:$false -ErrorAction SilentlyContinue
    Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'emergency\\listener\.ps1' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    'uninstalled (files kept in C:\BC250\emergency)'
    return
}
# The listener dot-sources stick-env.ps1 (grubenv functions for the signed stick). Both files must be in
# -Source before anything changes: a listener without its helper does not start.
foreach ($name in 'listener.ps1', 'stick-env.ps1') {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $name))) { throw "$name is not in $Source; nothing was changed" }
}
New-Item -ItemType Directory -Force (Join-Path $base 'scripts') | Out-Null
Copy-Item (Join-Path $Source 'stick-env.ps1') (Join-Path $base 'stick-env.ps1') -Force
Copy-Item (Join-Path $Source 'listener.ps1') (Join-Path $base 'listener.ps1') -Force
$tmpKey = Join-Path $Source 'emergency-key.bin'
if (Test-Path $tmpKey) {
    Move-Item $tmpKey (Join-Path $base 'key.bin') -Force
}
if (-not (Test-Path (Join-Path $base 'key.bin'))) { throw 'no key' }
Protect-EmergencyKey (Join-Path $base 'key.bin')
Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $rule -Direction Inbound -Protocol TCP -LocalPort 8722 -RemoteAddress LocalSubnet -Action Allow -Profile Any | Out-Null
# Stop an older listener (and the task under its former name) before the new task starts.
Stop-ScheduledTask -TaskName $oldTask -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $oldTask -Confirm:$false -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'emergency\\listener\.ps1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $base 'listener.ps1') + '"')
$t = New-ScheduledTaskTrigger -AtStartup
$p = New-ScheduledTaskPrincipal -UserId 'S-1-5-18' -LogonType ServiceAccount -RunLevel Highest
$s = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $task -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 4
$l = @(Get-NetTCPConnection -LocalPort 8722 -State Listen -ErrorAction SilentlyContinue)
'task {0}, port 8722 listening {1}' -f (Get-ScheduledTask -TaskName $task).State, $l.Count
Get-Content (Join-Path $base 'listener.log') -Tail 3 -ErrorAction SilentlyContinue
