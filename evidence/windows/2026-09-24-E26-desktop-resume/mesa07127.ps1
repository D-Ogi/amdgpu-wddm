$ErrorActionPreference='Stop'
$out='C:\BC250\m13\mesa07127'
if(Test-Path $out){throw 'Output exists'}
New-Item -ItemType Directory $out | Out-Null
$dll='C:\BC250\e26\branch-labels\bc250d3d.dll'
$sha=(Get-FileHash $dll).Hash
'umd_sha256='+$sha
if($sha -ne '440B2AB98A943E3F77849D5C1672271792C71F5C864D4BB1562E27B3C2DFC43F'){throw 'UMD mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object InstanceId -Like 'PCI\VEN_1002&DEV_13FE*')
if($gpu.Count -ne 1){throw 'Adapter identity'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
(Get-ItemProperty $class -Name UserModeDriverName).UserModeDriverName | Export-Clixml "$out\umd-before.xml"
Set-ItemProperty $class -Name UserModeDriverName -Value @('bc250umd.dll',$dll,$dll)
$start=Get-Date
Get-Process dwm | Stop-Process -Force
Start-Sleep -Seconds 15
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\scanout15.bmp"
& C:\BC250\m8\bc250kmd_cli.exe log summary
& C:\BC250\m8\bc250kmd_cli.exe log
Start-Sleep -Seconds 15
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\scanout30.bmp"
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit';StartTime=$start} -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,Message | Format-List
Get-ChildItem C:\BC250\e26\umdlogs | Where-Object LastWriteTime -ge $start | Select-Object Name,Length,LastWriteTime | Format-Table
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'mesa_trial_complete'
