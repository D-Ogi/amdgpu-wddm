param([ValidatePattern('^[a-z0-9-]+$')][string]$Run='normal-wait')
$ErrorActionPreference='Stop'
$root='C:\BC250\m12\system-icd'
if(Test-Path "$root\$Run"){throw 'Existing run'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$clock=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe clock read | Out-String
$clock | Set-Content "$root\preflight-clock.txt"
if($clock -notmatch 'MHz=1000 VID=116 temperature_mc=(\d+) ready=1' -or [int]$Matches[1] -ge 85000){throw 'Clock/thermal gate'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $root\system-worker.ps1 -Out $root\$Run"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Limited
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5)
Register-ScheduledTask -TaskName "BC250-M12-SystemProbe-$Run" -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask "BC250-M12-SystemProbe-$Run"
'STARTED'
