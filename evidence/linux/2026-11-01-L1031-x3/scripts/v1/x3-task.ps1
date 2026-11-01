# X3: install C:\BC250\x3\x3-clear.ps1 (already pushed) as the SYSTEM task "Lab boot counter reset", at startup + 3 min.
# The task name stays outside the kits' competing-task gate pattern 'BC250|DWM|G0|WSI'.
$ErrorActionPreference = 'Stop'
$name = 'Lab boot counter reset'
$script = 'C:\BC250\x3\x3-clear.ps1'
if (-not (Test-Path -LiteralPath $script)) { throw "missing $script" }
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$script`""
$trigger = New-ScheduledTaskTrigger -AtStartup
$trigger.Delay = 'PT3M'
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -StartWhenAvailable
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
$t = Get-ScheduledTask -TaskName $name
'{0} state={1} trigger={2} delay={3}' -f $t.TaskName, $t.State, $t.Triggers[0].CimClass.CimClassName, $t.Triggers[0].Delay
Start-ScheduledTask -TaskName $name
Start-Sleep -Seconds 8
Get-Content -LiteralPath 'C:\BC250\x3\x3-clear.log' -Tail 3
