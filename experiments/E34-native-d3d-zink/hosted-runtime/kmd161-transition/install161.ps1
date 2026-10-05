$ErrorActionPreference='Stop'
Start-Transcript -Path 'C:\BC250\m12\candidate07161\transition.log' -NoClobber | Out-Null
try {
$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) { throw 'Owner STOP requested' }
$out='C:\BC250\m12\candidate07161'
$pkg=Join-Path $out 'package-umd'
if(Test-Path (Join-Path $out 'old-driver.log')){throw 'Transition output already exists'}
$hashes=Get-Content (Join-Path $out 'package-hashes.json') -Raw | ConvertFrom-Json
foreach($set in @(@{name='candidate161';dir=$pkg},@{name='rollback160';dir=(Join-Path $out 'rollback160')})){
 foreach($file in $hashes.($set.name).PSObject.Properties){
  if((Get-FileHash (Join-Path $set.dir $file.Name)).Hash -ne $file.Value){throw 'Staged package mismatch'}
 }
}
if((Get-FileHash 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D'){throw 'Registered ICD baseline changed'}
$expected='40F7916FE24957666F392D5913EE70C7DCFEB1A23A121C39E6DB0ACE907B6FF1'
$dll='C:\BC250\m11\resource-close\bc250d3d.dll'
if((Get-FileHash (Join-Path $pkg 'bc250kmd.sys')).Hash -ne $expected){throw 'Candidate hash mismatch'}
if((Get-FileHash $dll).Hash -ne '8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA'){throw 'Desktop DLL mismatch'}
$retired=Get-Content 'C:\BC250\m9\candidate07136\legacy-retired.json' -Raw | ConvertFrom-Json
if($retired.LegacySmuResult -ne 50 -or $retired.ReaderSha -ne 'EB7FCA76B14A0C0817333509C8A88073571A3D607D5AB154F765E6C57873B625'){throw 'Legacy retirement proof missing'}
if((Get-FileHash C:\BC250\bc250rd\bc250rd.sys).Hash -ne $retired.ReaderSha -or (Get-Service bc250rd).Status -ne 'Running'){throw 'Reader identity/state changed'}
if((Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State -ne 'Disabled'){throw 'Legacy clock task active'}
if (@(Get-Process | Where-Object { $_.ProcessName -match 'deqp|vkcube|witcher3|PresentMon|state-control|texture-control|cross-process|flip-control|gfx-blt-control' }).Count) { throw 'Test process busy' }
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Expected one healthy GPU'}
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if($ver -ne '0.7.160.1'){throw 'Unexpected installed version'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x000700A0' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Unexpected loaded driver'}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'boot_before='+$boot
$dwmBefore=Get-Process dwm
'dwm_before='+$dwmBefore.Id+' start='+$dwmBefore.StartTime.ToString('s')
$imageBefore=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($imageBefore.StartsWith('\SystemRoot\')){$imageBefore=Join-Path $env:windir $imageBefore.Substring(12)}
if($imageBefore.StartsWith('\??\')){$imageBefore=$imageBefore.Substring(4)}
if((Get-FileHash -LiteralPath $imageBefore).Hash -ne '8E676C810190EF38BACF3E8332EDC2760E07844FD779E18CB32B22D24AC90218'){throw 'Old SYS mismatch'}


$raw=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
$raw
if($LASTEXITCODE -ne 0 -or $raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'Temperature unavailable'}
if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
& C:\BC250\m8\bc250kmd_cli.exe log | Out-File "$out\old-driver.log"
Get-ItemProperty $reg | Format-List | Out-File "$out\old-settings.log"
& C:\BC250\m9\checked-confirm456\bc250kmd_cli.exe confirm
if($LASTEXITCODE -ne 0){throw 'Guard confirmation failed'}
$savedParameters=@{}
$key=Get-Item $reg
foreach($name in $key.GetValueNames()){
 $savedParameters[$name]=@{value=$key.GetValue($name);kind=$key.GetValueKind($name).ToString()}
}
$savedParameters | ConvertTo-Json -Depth 4 | Set-Content "$out\parameters-before.json"
# Save exact class registration for recovery before disabling the adapter.
$classBefore='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
$classValues=Get-ItemProperty $classBefore
@{UserModeDriverName=@($classValues.UserModeDriverName);VulkanDriverName=@($classValues.VulkanDriverName)} | ConvertTo-Json | Set-Content "$out\registration-before.json"
if((Get-FileHash 'C:\BC250\m12\candidate07161\rollback160\bc250kmd.sys').Hash -ne '8E676C810190EF38BACF3E8332EDC2760E07844FD779E18CB32B22D24AC90218'){throw 'Rollback package missing or changed'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP requested before disable'}
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
New-ItemProperty $class -Name VulkanDriverName -PropertyType MultiString -Value @('C:\BC250\m10\wsi-final\radeon_icd.json') -Force | Out-Null
# INF closes gates. Restore the exact pre-transition driver parameters,
# including active native paging, rather than historical startup-test presets.
foreach($name in $savedParameters.Keys){
 New-ItemProperty $reg -Name $name -Value $savedParameters[$name].value -PropertyType $savedParameters[$name].kind -Force | Out-Null
}
if((Get-ItemProperty $reg).UnconfirmedStarts -ne 0){throw 'Unexpected guard state'}
New-ItemProperty $reg -Name EnableFullWddm -Value 2 -PropertyType DWord -Force | Out-Null
New-ItemProperty $reg -Name EnableGpuPresentBlit -Value 0 -PropertyType DWord -Force | Out-Null
New-ItemProperty $reg -Name EnableCddDwmInterop -Value 0 -PropertyType DWord -Force | Out-Null
New-ItemProperty $reg -Name EnableHandleIdentityProbe -Value 1 -PropertyType DWord -Force | Out-Null
'enable_begin='+(Get-Date).ToString('s')
& pnputil.exe /enable-device $gpu[0].InstanceId
if($LASTEXITCODE -ne 0){throw 'PnP enable failed'}
& "$out\wait-ready161.ps1"
& C:\BC250\m8\bc250kmd_cli.exe log | Out-File "$out\early-driver.log"
Start-Sleep -Seconds 8
& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read
& C:\BC250\m8\bc250kmd_cli.exe log | Tee-Object "$out\new-driver.log"
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
$info | Out-File "$out\new-info.log"
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x000700A1' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Candidate not active/full'}
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$ver
if($ver -ne '0.7.161.1' -or (Get-PnpDevice -InstanceId $gpu[0].InstanceId).Status -ne 'OK'){throw 'Candidate not healthy'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne $expected){throw 'Installed SYS mismatch'}
# Leave the genuine newly admitted budget for monitor automatic confirmation.
$afterBoot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
'boot_after='+$afterBoot
if($afterBoot -ne $boot){throw 'Unexpected OS restart'}
Get-Process dwm | Select-Object Id,StartTime,Responding | Format-Table
Get-ItemProperty $reg | Select-Object EnableFullWddm,EnableSdmaIbControl,EnableSdmaVaControl,UnconfirmedStarts,EnablePresentBlit,EnableDcnWrite,EnableVidPnFlip | Format-List
$dwmAfter=Get-Process dwm
'dwm_retained='+($dwmAfter.Id -eq $dwmBefore.Id -and $dwmAfter.StartTime -eq $dwmBefore.StartTime)
& C:\BC250\m9\candidate07136\client\bc250kmd_cli.exe clock read
if($LASTEXITCODE -ne 0){throw 'Native paired telemetry failed'}
& C:\BC250\bc250rd\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Native clock positive control failed'}
'candidate161_diagnostic_ready_native_paging_preserved'

} finally { Stop-Transcript | Out-Null }
