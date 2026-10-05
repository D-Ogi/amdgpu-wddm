$ErrorActionPreference='Stop'
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP'}
$cli='C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'GPU state'}
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$expected='C:\BC250\m13\mesa-quiet147-umd\bc250d3d.dll'
if((Get-FileHash $expected).Hash -ne 'FF864DB8CBD6512DDD5538941A6332A9F85AF0D6EA287A5C3FC4A2C2C771E6D5'){throw 'New DLL hash'}
Set-ItemProperty $class -Name UserModeDriverName -Value @('bc250umd.dll',$expected,$expected)
$info=& $cli info | Out-String
if($info -notmatch '0x00070093'){throw 'KMD mismatch'}
& $cli health read
& $cli confirm
if($LASTEXITCODE -ne 0){throw 'Confirmation failed'}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
& pnputil.exe /disable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'Disable failed'}
Start-Sleep -Seconds 2
if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data -ne 22){throw 'Not disabled'}
& pnputil.exe /enable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'Enable failed'}
Start-Sleep -Seconds 8
& $cli health read
Get-Process dwm | Stop-Process -Force
Start-Sleep -Seconds 8
$after=Get-Process dwm
'new_dwm='+$after.Id
$modules=@($after.Modules | Where-Object ModuleName -eq 'bc250d3d.dll')
foreach($m in $modules){'loaded_umd='+$m.FileName;'loaded_hash='+(Get-FileHash $m.FileName).Hash}
if($modules.Count -ne 1 -or $modules[0].FileName -ne $expected){throw 'Expected module not loaded'}
& $cli health read
& $cli clock read
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
'paired147_active'
