$ErrorActionPreference='Stop'
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$out='C:\BC250\m9\candidate07140'
$settings=Get-ItemProperty $reg
$settings | Select-Object EnableMmio,EnableNativeSmu,EnableVram,EnableGart,EnablePsp,EnableGfx,StageHistory,UnconfirmedStarts | Format-List
if($settings.EnableMmio -ne 0 -or $settings.EnableNativeSmu -ne 1){throw 'Unexpected failure cause; inspect'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or (Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data -ne '0.7.140.1'){throw 'Not140'}
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
& pnputil.exe /disable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'Disable failed'}
if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Not disabled'}
# INF writes diagnostic gates to0. Apply the authorized profile AFTER package installation.
foreach($n in @('EnableNativeSmu','EnableMmio','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','KeepLog','EnableDcnWrite','EnableVidPnFlip','EnablePresentBlit','EnableSdmaIbControl','EnableSdmaVaControl','EnableFullWddm')){New-ItemProperty $reg $n -Value 1 -PropertyType DWord -Force | Out-Null}
foreach($n in @('EnableMmioWrite','EnableRlcReloadReset','UnconfirmedStarts')){New-ItemProperty $reg $n -Value 0 -PropertyType DWord -Force | Out-Null}
& pnputil.exe /enable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'Enable failed'}
Start-Sleep -Seconds 6
& C:\BC250\m8\bc250kmd_cli.exe log | Out-File "$out\startup-profile-corrected.log"
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x0007008C' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Full140 notready'}
foreach($n in @('EnableSdmaIbControl','EnableSdmaVaControl')){New-ItemProperty $reg $n -Value 0 -PropertyType DWord -Force | Out-Null}
& C:\BC250\m8\bc250kmd_cli.exe confirm
& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read
'full140_profile_ready='+(Get-Date).ToString('s')
