$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Unhealthy adapter'}
if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data -ne '0.7.144.1'){throw 'Wrong version'}
$out='C:\BC250\m9\candidate07144\osboot'
if(Test-Path $out){throw 'Checkpoint exists'}
New-Item -ItemType Directory $out | Out-Null
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Get-ItemProperty $reg | Format-List | Out-File "$out\before-settings.log"
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before-driver.log"
& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read
if($LASTEXITCODE -ne 0){throw 'Native clock unavailable'}
& C:\BC250\m8\bc250kmd_cli.exe confirm
if($LASTEXITCODE -ne 0){throw 'Cannot confirm successful warm control'}
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters',$true)
try {
 if([int]$key.GetValue('UnconfirmedStarts',99) -ne 0){throw 'Unexpected budget'}
 foreach($name in @('EnableNativeSmu','EnableMmio','EnableVram','EnableVramWrite','EnableGart','EnablePsp','EnableGfx','EnableIh','EnableGpuVa','EnableGpuSubmit','EnablePagingNode','EnableDcnWrite','EnableVidPnFlip','EnablePresentBlit')){if([int]$key.GetValue($name,0) -ne 1){throw "Missing profile: $name"}}
 $key.SetValue('EnableFullWddm',2,[Microsoft.Win32.RegistryValueKind]::DWord)
 $key.Flush()
 if([int]$key.GetValue('EnableFullWddm',0) -ne 2){throw 'Policy write mismatch'}
}finally{$key.Dispose()}
'policy=2 persistent full table; strict per-start durability required'
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'os_restart_requested='+(Get-Date).ToString('s')
& shutdown.exe /r /t 5 /d p:0:0 /c 'BC250 M9: verify production WDDM startup and durable guard'
if($LASTEXITCODE -ne 0){throw 'Restart request failed'}
