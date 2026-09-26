$ErrorActionPreference='Stop'
$out='C:\BC250\m9\resume146-s4'
'now='+(Get-Date).ToString('o')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
foreach($n in @('hibernate-request.txt','hibernate-return.txt','ready.txt','probe.exit')){
 if(Test-Path "$out\$n"){$n;Get-Content "$out\$n"}
}
'processes'
Get-Process gpu-residency-probe,dwm,bc250mon -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,StartTime | ConvertTo-Json
'health'
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
'power_events'
Get-WinEvent -FilterHashtable @{LogName='System';StartTime=(Get-Date).AddMinutes(-15)} -ErrorAction SilentlyContinue | Where-Object {$_.ProviderName -in @('Microsoft-Windows-Kernel-Power','Microsoft-Windows-Power-Troubleshooter','Microsoft-Windows-Kernel-Boot','Microsoft-Windows-WER-SystemErrorReporting')} | Select-Object TimeCreated,Id,ProviderName,Message | Format-List
'probe_tail'
Get-Content "$out\probe.out" -Tail 8
'postwake_observation_complete'
