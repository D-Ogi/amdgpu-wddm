$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
$out='C:\BC250\m9\candidate07140'
$pkg="$out\package-umd"
$expected='BBDB196F726226CC1B435F94EDCDD02E458F28F6507544B26EFF0BE5CE4B9252'
if((Get-FileHash "$pkg\bc250kmd.sys").Hash -ne $expected){throw 'Candidate mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1){throw 'GPU identity ambiguous'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
$problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
'previous_version='+$version+' problem='+$problem
if($version -ne '0.7.136.1' -or $problem -ne 43){throw 'Unexpected recovery entry'}
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Get-ItemProperty $reg | Format-List | Out-File "$out\before-settings.log"
if((Get-ScheduledTask 'BC250 GPU clock 1000MHz 820mV').State -ne 'Disabled'){throw 'Legacy writer active'}
$temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
$temp
if($LASTEXITCODE -ne 0 -or $temp -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -or [double]$Matches[1] -ge 85){throw 'Temperature unavailable/high'}
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& pnputil.exe /disable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'Disable failed'}
if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Not disabled'}
# One diagnostic retry of a known Code43 start, preserved above; owner authorizes lab recovery.
New-ItemProperty $reg UnconfirmedStarts -Value 0 -PropertyType DWord -Force | Out-Null
foreach($n in @('EnableNativeSmu','EnableMmio','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','KeepLog','EnableDcnWrite','EnableVidPnFlip','EnablePresentBlit','EnableSdmaIbControl','EnableSdmaVaControl','EnableFullWddm')){New-ItemProperty $reg $n -Value 1 -PropertyType DWord -Force | Out-Null}
foreach($n in @('EnableMmioWrite','EnableRlcReloadReset')){New-ItemProperty $reg $n -Value 0 -PropertyType DWord -Force | Out-Null}
& pnputil.exe /add-driver "$pkg\bc250kmd.inf" /install
if($LASTEXITCODE -ne 0){throw 'Install failed'}
if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Unexpected autoenable; inspect'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$dll='C:\BC250\m13\mesa-main-umd\bc250d3d.dll'
if((Get-FileHash $dll).Hash -ne 'D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D'){throw 'UMD mismatch'}
Set-ItemProperty $class UserModeDriverName -Value @('bc250umd.dll',$dll,$dll)
New-ItemProperty $reg EnableFullWddm -Value 1 -PropertyType DWord -Force | Out-Null
& pnputil.exe /enable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'Enable failed'}
Start-Sleep -Seconds 6
& C:\BC250\m8\bc250kmd_cli.exe log | Out-File "$out\startup.log"
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info;$info | Out-File "$out\info.log"
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x0007008C' -or $info -notmatch 'FULL WDDM TABLE'){throw '140 full not ready'}
New-ItemProperty $reg EnableSdmaIbControl -Value 0 -PropertyType DWord -Force | Out-Null
New-ItemProperty $reg EnableSdmaVaControl -Value 0 -PropertyType DWord -Force | Out-Null
& C:\BC250\m8\bc250kmd_cli.exe confirm
if($LASTEXITCODE -ne 0){throw 'Confirm failed'}
& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read
if($LASTEXITCODE -ne 0){throw 'Native clock unavailable'}
Get-Process dwm | Select-Object Id,StartTime,Responding | Format-Table
'candidate140_ready='+(Get-Date).ToString('s')
