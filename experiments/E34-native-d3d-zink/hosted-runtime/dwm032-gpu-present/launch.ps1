$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted032'
$task='BC250-G0-DwmRun032'
if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Existing task'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d\run.ps1"
$principal=New-ScheduledTaskPrincipal -UserId SYSTEM -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)) | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 2
Get-ScheduledTask -TaskName $task | Select-Object TaskName,State | ConvertTo-Json
