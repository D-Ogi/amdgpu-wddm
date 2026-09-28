. "$PSScriptRoot\common.ps1"
Assert-Stage
Assert-StopThermal
foreach($name in @($workerTask,$watchdogTask)){if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){throw 'Existing trial task'}}
foreach($file in @('done.json','watchdog-ready.json','baseline-umd.dll','baseline-icd.dll','marker.txt')){if(Test-Path "$d\$file"){throw 'Existing trial artifact'}}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(!$who){throw 'No interactive user'}
$watchAction=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d\watchdog.ps1"
Register-ScheduledTask -TaskName $watchdogTask -Action $watchAction -Principal (New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest) -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3))|Out-Null
Start-ScheduledTask $watchdogTask
$deadline=[DateTime]::UtcNow.AddSeconds(10)
while(!(Test-Path "$d\watchdog-ready.json") -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
if(!(Test-Path "$d\watchdog-ready.json")){throw 'Watchdog did not arm'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d\worker.ps1"
Register-ScheduledTask -TaskName $workerTask -Action $action -Principal (New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest) -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 2))|Out-Null
Start-ScheduledTask $workerTask
Get-ScheduledTask -TaskName $workerTask,$watchdogTask|Select-Object TaskName,State|ConvertTo-Json
