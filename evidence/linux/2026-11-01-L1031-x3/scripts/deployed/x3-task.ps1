# LAB ONLY. Install for an explicit existing administrator account, never the
# remote caller (which may be SYSTEM). InteractiveToken stores no password.
# No task is started by this installer; an actual logon triggers observation.
param([Parameter(Mandatory=$true)][string]$LogonUser)
$ErrorActionPreference = 'Stop'
if ($LogonUser -match '["\r\n]' -or $LogonUser -match '^(SYSTEM|NT AUTHORITY\\SYSTEM|S-1-5-18)$') { throw 'specify the real interactive administrator account' }
$account = New-Object Security.Principal.NTAccount($LogonUser)
$sid = $account.Translate([Security.Principal.SecurityIdentifier]).Value
if ($sid -eq 'S-1-5-18') { throw 'SYSTEM cannot produce an interactive witness' }
$name = 'Lab boot counter reset'
$script = Join-Path $PSScriptRoot 'x3-clear.ps1'
foreach ($file in @($script, (Join-Path $PSScriptRoot 'x3-env.ps1'), (Join-Path $PSScriptRoot 'x3-storage.ps1'), (Join-Path $PSScriptRoot 'x3-session.ps1'), (Join-Path $PSScriptRoot 'x3-process.ps1'), (Join-Path $PSScriptRoot 'bounded-child.exe'), (Join-Path $PSScriptRoot 'bounded-child.sha256'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "missing $file" }
}
# conhost --headless: no console window in the user's desktop (a flash would take focus during desktop tests).
$action = New-ScheduledTaskAction -Execute "$env:WINDIR\System32\conhost.exe" -Argument "--headless `"$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe`" -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $sid
$trigger.Delay = 'PT90S'
$principal = New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -StartWhenAvailable -MultipleInstances IgnoreNew -RestartCount 2 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
$t = Get-ScheduledTask -TaskName $name
if ($t.Actions[0].Arguments -cne $action.Arguments -or @($t.Triggers).Count -ne 1 -or $t.Principal.LogonType -ne 'Interactive') { throw 'registered task readback differs' }
'X3 interactive-logon task registered; NOT started, counter NOT cleared. Boot/fallback acceptance remains pending.'
'No logon means no automatic counter acknowledgement; intentional CPU recovery is admitted only by actual live desktop probes.'
