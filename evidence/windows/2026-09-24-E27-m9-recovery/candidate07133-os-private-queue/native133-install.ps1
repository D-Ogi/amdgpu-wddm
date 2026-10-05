$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }
$out='C:\BC250\m9\candidate07133'
$pkg=Join-Path $out 'package-umd'
$expected='37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87'
$dll='C:\BC250\m13\mesa-main-umd\bc250d3d.dll'
if((Get-FileHash (Join-Path $pkg 'bc250kmd.sys')).Hash -ne $expected){throw 'Candidate hash mismatch'}
if((Get-FileHash $dll).Hash -ne 'D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D'){throw 'Desktop DLL mismatch'}
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Expected one healthy GPU'}
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if($ver -ne '0.7.132.1'){throw 'Unexpected installed version'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070084' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Unexpected loaded driver'}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'boot_before='+$boot
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock preflight failed'}
$raw=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
$raw
if($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'Temperature unavailable'}
if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
& C:\BC250\m8\bc250kmd_cli.exe log | Out-File "$out\old-driver.log"
Get-ItemProperty $reg | Format-List | Out-File "$out\old-settings.log"
& C:\BC250\m8\bc250kmd_cli.exe confirm
if($LASTEXITCODE -ne 0){throw 'Guard confirmation failed'}
'disable_begin='+(Get-Date).ToString('s')
& pnputil.exe /disable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'PnP disable failed'}
Start-Sleep -Seconds 3
$problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
'disabled_problem='+$problem
if($problem -ne 22){throw 'Device did not disable'}
'install_begin='+(Get-Date).ToString('s')
& pnputil.exe /add-driver (Join-Path $pkg 'bc250kmd.inf') /install
if($LASTEXITCODE -ne 0){throw ('Install requires inspection, code='+$LASTEXITCODE)}
$problem=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
if($problem -ne 22){throw 'Install unexpectedly enabled device; inspect before proceeding'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
Set-ItemProperty $class -Name UserModeDriverName -Value @('bc250umd.dll',$dll,$dll)
foreach($n in @('EnableMmio','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','KeepLog','EnableDcnWrite','EnableVidPnFlip','EnablePresentBlit','EnableSdmaIbControl','EnableSdmaVaControl')){
 New-ItemProperty $reg -Name $n -Value 1 -PropertyType DWord -Force | Out-Null
}
foreach($n in @('EnableMmioWrite','EnableRlcReloadReset')){New-ItemProperty $reg -Name $n -Value 0 -PropertyType DWord -Force | Out-Null}
if((Get-ItemProperty $reg).UnconfirmedStarts -ne 0){throw 'Unexpected guard state'}
New-ItemProperty $reg -Name EnableFullWddm -Value 1 -PropertyType DWord -Force | Out-Null
'enable_begin='+(Get-Date).ToString('s')
& pnputil.exe /enable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'PnP enable failed'}
Start-Sleep -Seconds 8
& C:\BC250\m8\bc250kmd_cli.exe log | Tee-Object "$out\new-driver.log"
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
$info | Out-File "$out\new-info.log"
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070085' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Candidate not active/full'}
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$ver
if($ver -ne '0.7.133.1' -or (Get-PnpDevice -InstanceId $gpu[0].InstanceId).Status -ne 'OK'){throw 'Candidate not healthy'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne $expected){throw 'Installed SYS mismatch'}
New-ItemProperty $reg -Name EnableSdmaIbControl -Value 0 -PropertyType DWord -Force | Out-Null
New-ItemProperty $reg -Name EnableSdmaVaControl -Value 0 -PropertyType DWord -Force | Out-Null
& C:\BC250\m8\bc250kmd_cli.exe confirm
if($LASTEXITCODE -ne 0){throw 'Guard confirmation failed'}
$afterBoot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'boot_after='+$afterBoot
if($afterBoot -ne $boot){throw 'Unexpected OS restart'}
Get-Process dwm | Select-Object Id,StartTime,Responding | Format-Table
Get-ItemProperty $reg | Select-Object EnableFullWddm,EnableSdmaIbControl,EnableSdmaVaControl,UnconfirmedStarts,EnablePresentBlit,EnableDcnWrite,EnableVidPnFlip | Format-List
& C:\BC250\m8\bc250kmd_cli.exe fbdump "$out\scanout.bmp"
if($LASTEXITCODE -ne 0){throw 'Scanout capture failed'}
'candidate133_transition_complete'
