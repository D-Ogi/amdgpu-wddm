param([string]$RunId,[switch]$Control)
$ErrorActionPreference='Stop'
if($RunId -notmatch '^piglit-gpu[0-9]+$'){throw 'Invalid run ID'}
if(@(Get-ScheduledTask 'BC250-M12-*' | Where-Object {$_.State -in @('Running','Queued')}).Count){throw 'M12 task active'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$taskName="BC250-M12-$RunId"
if(Get-ScheduledTask $taskName -ErrorAction SilentlyContinue){throw 'Task exists'}
$arguments="-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\BC250\m12\piglit-gpu-worker.ps1 -RunId $RunId"
if($Control){$arguments+=' -Control'}
$action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument $arguments
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::FromHours(12)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask $taskName
@{task=$taskName;state=(Get-ScheduledTask $taskName).State.ToString()} | ConvertTo-Json
