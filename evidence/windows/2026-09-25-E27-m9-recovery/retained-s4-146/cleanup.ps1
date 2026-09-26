$ErrorActionPreference='Stop'
foreach($n in @('BC250-M9-Resume146Probe','BC250-M9-Resume146Hibernate')){
 $t=Get-ScheduledTask -TaskName $n
 if($t.State -eq 'Running'){throw 'Task remains active; do not remove'}
 Unregister-ScheduledTask -TaskName $n -Confirm:$false
}
New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -Value 0 -PropertyType DWord -Force | Out-Null
'full_S4_enabled_fast_startup_disabled'
'now='+(Get-Date).ToString('o')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe health read
& C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe clock read
Get-Process dwm,bc250mon,LogonUI -ErrorAction SilentlyContinue | Select-Object ProcessName,Id,StartTime | Format-Table
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object UnconfirmedStarts,EnableFullWddm | ConvertTo-Json
'cleanup_complete'
