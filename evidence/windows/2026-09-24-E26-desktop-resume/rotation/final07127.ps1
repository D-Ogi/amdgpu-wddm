$ErrorActionPreference='Stop'
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit';StartTime=[datetime]'2026-09-24T12:32:53'} -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,Message | Format-List
$gpu=@(Get-PnpDevice -Class Display | Where-Object InstanceId -Like 'PCI\VEN_1002&DEV_13FE*')
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
Get-ItemProperty $class | Select-Object UserModeDriverName | Format-List
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object Enable*,LastStage,UnconfirmedStarts | Format-List
& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1
& C:\BC250\m8\bc250kmd_cli.exe info
& C:\BC250\m8\bc250kmd_cli.exe dcn
& C:\BC250\m8\bc250kmd_cli.exe log summary
& C:\BC250\m8\bc250kmd_cli.exe log
& C:\BC250\m8\bc250kmd_cli.exe confirm
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'final_time='+(Get-Date).ToString('s')
'initialized_desktop_retained'
