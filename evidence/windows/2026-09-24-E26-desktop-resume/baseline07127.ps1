$ErrorActionPreference='Stop'
$out='C:\BC250\m13\baseline07127'
if(Test-Path $out){throw 'Output exists'}
New-Item -ItemType Directory $out | Out-Null
$gpu=@(Get-PnpDevice -Class Display | Where-Object InstanceId -Like 'PCI\VEN_1002&DEV_13FE*')
if($gpu.Count -ne 1){throw 'Adapter identity'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
Get-ItemProperty $class | Select-Object UserModeDriverName,UserModeDriverNameWow,InstalledDisplayDrivers | Format-List
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' | Select-Object Enable*,LastStage,UnconfirmedStarts | Format-List
Get-Process dwm | Select-Object Id,StartTime,CPU,Responding | Format-Table
Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Dwminit'} -MaxEvents 5 -ErrorAction SilentlyContinue | Select-Object TimeCreated,Id,Message | Format-List
& C:\BC250\m8\bc250kmd_cli.exe info
& C:\BC250\m8\bc250kmd_cli.exe log summary
& C:\BC250\m8\bc250kmd_cli.exe log
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\scanout.bmp"
if($LASTEXITCODE -ne 0){throw 'Scanout capture failed'}
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'baseline_complete'
