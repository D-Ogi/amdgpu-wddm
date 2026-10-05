$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) { throw 'Owner STOP requested' }
$out='C:\BC250\m9\candidate07134-guard'
if(Test-Path $out){throw 'Output already exists'}
New-Item -ItemType Directory $out | Out-Null
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'No healthy GPU'}
if((Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data -ne '0.7.134.1'){throw 'Wrong driver'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
if((Get-FileHash -LiteralPath $image).Hash -ne '8DC2172B803F965F6F91C1FAB7116DEB777369F9A4A6FA85FD9441B2F1597534'){throw 'Wrong SYS'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070086' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Full134 unavailable'}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"
if($LASTEXITCODE -ne 0){throw 'Summary failed'}
# Expected no-write refusal: active VidPn owns OTG0 in M431/134.
$result=& C:\BC250\m8\bc250kmd_cli.exe dcnflip restore | Out-String
$code=$LASTEXITCODE
$result
$result | Out-File "$out\refusal.log"
'escape_exit='+$code
if($code -ne 3 -or $result -notmatch 'REFUSED, NTSTATUS 0x80000011' -or $result -notmatch 'diagnostic flip refused while VidPn owns OTG0'){throw 'Unexpected diagnostic result'}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
if($LASTEXITCODE -ne 0){throw 'Summary failed'}
Get-Process dwm | Select-Object Id,StartTime,Responding | Format-Table
'diagnostic_ownership_control_pass'
'final_time='+(Get-Date).ToString('s')
