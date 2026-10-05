$ErrorActionPreference='Continue'
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'now='+(Get-Date).ToString('s')
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
$gpu | Select-Object Status,FriendlyName | Format-Table
if($gpu.Count -eq 1){'version='+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data;'problem='+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data}
Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters | Select-Object EnableFullWddm,UnconfirmedStarts,LastStage,StageHistory | Format-List
& C:\BC250\m8\bc250kmd_cli.exe info
& C:\BC250\m8\bc250kmd_cli.exe log summary
& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read
Get-Process dwm,bc250mon -ErrorAction SilentlyContinue | Select-Object Name,Id,StartTime,Responding | Format-Table
