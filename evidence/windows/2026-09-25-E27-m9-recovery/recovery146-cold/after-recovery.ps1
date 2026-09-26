$ErrorActionPreference='Stop'
'now='+(Get-Date).ToString('o')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
foreach($n in @('BC250-OneTimeLogin146','BC250-OneTimeUnlock146')){
 $task=Get-ScheduledTask -TaskName $n -ErrorAction SilentlyContinue
 if($task){if($task.State -eq 'Running'){Stop-ScheduledTask $n};Unregister-ScheduledTask $n -Confirm:$false}
}
Get-Process LoginHelper146 -ErrorAction SilentlyContinue | Stop-Process -Force
'login_helpers_stopped_and_tasks_removed'
Get-Process dwm,bc250mon,LogonUI -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,StartTime | Format-Table
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
'light_cpu_samples'
for($i=0;$i -lt 6;$i++){
 Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" | Select-Object Name,PercentProcessorTime,PercentDPCTime,PercentInterruptTime,InterruptsPersec | ConvertTo-Json -Compress
 Start-Sleep -Seconds 1
}
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
'after_recovery_complete'
