$ErrorActionPreference='Stop'
$out='C:\BC250\m9\resume146-s4'
Get-WinEvent -FilterHashtable @{LogName='System';StartTime=[datetime]'2026-09-25T03:01:00'} | Where-Object {$_.ProviderName -in @('Microsoft-Windows-Kernel-Power','Microsoft-Windows-Power-Troubleshooter','Microsoft-Windows-Kernel-Boot','Microsoft-Windows-WER-SystemErrorReporting')} | ForEach-Object {$_.ToXml()} | Set-Content "$out\power-events.xml" -Encoding UTF8
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object UnconfirmedStarts,EnableFullWddm | ConvertTo-Json
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe clock read
Select-String -Path "$out\resumed-driver.log" -Pattern 'snapshot|lines printed|power:|retained resume' | ForEach-Object {$_.Line}
Get-ScheduledTask -TaskName BC250-M9-Resume146Probe,BC250-M9-Resume146Hibernate | Select-Object TaskName,State | Format-Table
Get-Process dwm,bc250mon,LogonUI -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,StartTime | Format-Table
'collected'
