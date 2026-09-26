$ErrorActionPreference='Stop'
$out='C:\BC250\m9\shader-buffer-alias119'
if(Test-Path $out){throw "Output directory already exists"}
New-Item -ItemType Directory $out | Out-Null
Copy-Item -LiteralPath 'C:\BC250\tmp\shader-buffer-alias-m383\shader-coherency-probe.exe' -Destination "$out\shader-coherency-probe.exe"
$probeHash=(Get-FileHash "$out\shader-coherency-probe.exe").Hash
'probe_hash='+$probeHash
if($probeHash -ne '6D69F6D4D77E702B6ABF0FA24B57F799A0CA88C26B3C290BC3A3C54B986FF76D'){throw 'Probe mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Device not healthy'}
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$ver
if($ver -ne '0.7.119.1'){throw 'Unexpected driver version'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$installed=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$installed
if($installed -ne '4374CB20857D0CA5D09D8EFCF61F5D35718633D568571EF41FE717900E15236E'){throw 'SYS mismatch'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070077' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Actual loaded full driver not confirmed'}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock verification failed'}
$temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
$temp.Trim()
if($LASTEXITCODE -ne 0 -or $temp -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'No temperature'}
if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"
$hash=(Get-FileHash C:\BC250\m9\cache-intent-v2\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlcY2FjaGUtaW50ZW50LXYyXHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IEJDMjUwX1RFU1RfRVZJQ1RfT05fVU5NQVA9CnNldCBQQVRIPUM6XEJDMjUwXG04OyVQQVRIJQpDOlxCQzI1MFxtOVxzaGFkZXItYnVmZmVyLWFsaWFzMTE5XHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiAtLW1lbW9yeS10eXBlIDMgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItYnVmZmVyLWFsaWFzMTE5XGJhc2VsaW5lLm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItYnVmZmVyLWFsaWFzMTE5XGJhc2VsaW5lLmVycgpzZXQgcHJvYmVfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlcHJvYmVfZXhpdCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItYnVmZmVyLWFsaWFzMTE5XGJhc2VsaW5lLmV4aXQKaWYgbm90ICIlcHJvYmVfZXhpdCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcc2hhZGVyLWNvaGVyZW5jeS1wcm9iZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tbWVtb3J5LXR5cGUgMyAtLWFsaWFzLWludGVybWVkaWF0ZSA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcYWxpYXMub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcYWxpYXMuZXJyCnNldCBwcm9iZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVwcm9iZV9leGl0JSA+IEM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcYWxpYXMuZXhpdAppZiBub3QgIiVwcm9iZV9leGl0JSI9PSIwIiBleGl0IC9iIDEKQzpcQkMyNTBcbTlcc2hhZGVyLWJ1ZmZlci1hbGlhczExOVxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSAzIC0tYWxpYXMtaW50ZXJtZWRpYXRlIC0tZGlzam9pbnQtYWxpYXMtY29udHJvbCA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcZGlzam9pbnQub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcZGlzam9pbnQuZXJyCnNldCBwcm9iZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVwcm9iZV9leGl0JSA+IEM6XEJDMjUwXG05XHNoYWRlci1idWZmZXItYWxpYXMxMTlcZGlzam9pbnQuZXhpdAppZiBub3QgIiVwcm9iZV9leGl0JSI9PSIxIiBleGl0IC9iIDEKZXhpdCAvYiAwCg==')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-ShaderBufferAlias119 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-ShaderBufferAlias119
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-ShaderBufferAlias119
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-ShaderBufferAlias119).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}

foreach($name in @('baseline','alias','disjoint')) {
 $code=[int](Get-Content "$out\$name.exit")
 "$name exit=$code"
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'cache-intent-v2\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){throw 'Missing actual ICD witness'}
 $result=Get-Content "$out\$name.out" -Raw
 if($name -eq 'disjoint') {
  if($code -ne 1 -or $result -notmatch 'COHERENCY completed=0 mismatches=1' -or $result -notmatch 'same_memory=0 negative_control=1'){throw 'Negative control failed'}
 } else {
  if($code -ne 0 -or $result -notmatch 'COHERENCY completed=16 mismatches=0'){throw 'Positive test failed'}
  if($name -eq 'alias' -and $result -notmatch 'BUFFER_ALIAS distinct_buffers=1 same_memory=1 offset=0'){throw 'Missing alias witness'}
 }
 Get-Content "$out\$name.out" -Tail 3
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-ShaderBufferAlias119 -Confirm:$false
'memory_types_run_complete'

'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
