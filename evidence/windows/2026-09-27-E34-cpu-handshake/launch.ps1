$ErrorActionPreference='Stop'
$out='C:\BC250\m13\redirblt-cpu001';$name='BC250-G0-RedirCpu001'
if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){throw 'Task already exists'}
if(Test-Path "$out\start.json"){throw 'Run exists'}
$who=(Get-CimInstance Win32_ComputerSystem).UserName
if(!$who){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $out\run.ps1"
$principal=New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $name
Get-ScheduledTask -TaskName $name | Select-Object TaskName,State | ConvertTo-Json
