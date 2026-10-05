$ErrorActionPreference='Stop'
$out='C:\BC250\m9\shader-alias-eviction119'
if(Test-Path $out){throw "Output directory already exists"}
New-Item -ItemType Directory $out | Out-Null
Copy-Item -LiteralPath 'C:\BC250\tmp\shader-alias-eviction-m384\shader-coherency-probe.exe' -Destination "$out\shader-coherency-probe.exe"
$probeHash=(Get-FileHash "$out\shader-coherency-probe.exe").Hash
'probe_hash='+$probeHash
if($probeHash -ne 'F3721C0600BAB71EDB83A53AB0468086DC5B24D6880BBA476232E2BFBDE7FB5B'){throw 'Probe mismatch'}
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
$hash=(Get-FileHash C:\BC250\m9\shader-eviction-icd\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne 'EA70D44045BF77867C9BE50EFB311E7CE7C83D01F4B527FB190E5BC8BD3F8306'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uLWljZFxyYWRlb25faWNkLmpzb24Kc2V0IFZLX0lDRF9GSUxFTkFNRVM9JVZLX0RSSVZFUl9GSUxFUyUKc2V0IFZLX0xPQURFUl9ERUJVRz1kcml2ZXIKc2V0IE1FU0FfU0hBREVSX0NBQ0hFX0RJU0FCTEU9dHJ1ZQpzZXQgQkMyNTBfVFJBQ0VfU1VCTUlUUz0wCnNldCBCQzI1MF9JQl9EV09SRFM9CnNldCBCQzI1MF9URVNUX0VWSUNUX09OX1VOTUFQPQpzZXQgUEFUSD1DOlxCQzI1MFxtODslUEFUSCUKQzpcQkMyNTBcbTlcc2hhZGVyLWFsaWFzLWV2aWN0aW9uMTE5XHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiAtLW1lbW9yeS10eXBlIDMgLS1hbGlhcy1pbnRlcm1lZGlhdGUgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItYWxpYXMtZXZpY3Rpb24xMTlcYmFzZWxpbmUub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1hbGlhcy1ldmljdGlvbjExOVxiYXNlbGluZS5lcnIKc2V0IHByb2JlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJXByb2JlX2V4aXQlID4gQzpcQkMyNTBcbTlcc2hhZGVyLWFsaWFzLWV2aWN0aW9uMTE5XGJhc2VsaW5lLmV4aXQKaWYgbm90ICIlcHJvYmVfZXhpdCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XHNoYWRlci1hbGlhcy1ldmljdGlvbjExOVxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSAzIC0tYWxpYXMtaW50ZXJtZWRpYXRlIC0tZXZpY3QtYWxpYXMgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItYWxpYXMtZXZpY3Rpb24xMTlcZXZpY3Qub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1hbGlhcy1ldmljdGlvbjExOVxldmljdC5lcnIKc2V0IHByb2JlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJXByb2JlX2V4aXQlID4gQzpcQkMyNTBcbTlcc2hhZGVyLWFsaWFzLWV2aWN0aW9uMTE5XGV2aWN0LmV4aXQKaWYgbm90ICIlcHJvYmVfZXhpdCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XHNoYWRlci1hbGlhcy1ldmljdGlvbjExOVxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSAzIC0tYWxpYXMtaW50ZXJtZWRpYXRlIC0tZXZpY3QtYWxpYXMgLS1zdGFsZS1pbnB1dC1jb250cm9sIDwgTlVMID4gQzpcQkMyNTBcbTlcc2hhZGVyLWFsaWFzLWV2aWN0aW9uMTE5XHN0YWxlLm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItYWxpYXMtZXZpY3Rpb24xMTlcc3RhbGUuZXJyCnNldCBwcm9iZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVwcm9iZV9leGl0JSA+IEM6XEJDMjUwXG05XHNoYWRlci1hbGlhcy1ldmljdGlvbjExOVxzdGFsZS5leGl0CmlmIG5vdCAiJXByb2JlX2V4aXQlIj09IjEiIGV4aXQgL2IgMQpleGl0IC9iIDAK')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-ShaderAliasEviction119 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-ShaderAliasEviction119
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-ShaderAliasEviction119
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-ShaderAliasEviction119).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}

foreach($name in @('baseline','evict','stale')) {
 $code=[int](Get-Content "$out\$name.exit")
 "$name exit=$code"
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'shader-eviction-icd\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){throw 'Missing actual ICD witness'}
 $result=Get-Content "$out\$name.out" -Raw
 if($result -notmatch 'BUFFER_ALIAS distinct_buffers=1 same_memory=1 offset=0'){throw 'Missing alias witness'}
 if($name -eq 'stale') {
  if($code -ne 1 -or $result -notmatch 'COHERENCY completed=1 mismatches=1'){throw 'Negative control failed'}
 } else {
  if($code -ne 0 -or $result -notmatch 'COHERENCY completed=16 mismatches=0'){throw 'Positive test failed'}
 }
 $expected=if($name -eq 'baseline'){0}elseif($name -eq 'evict'){3}else{1}
 $departures=[regex]::Matches($trace,'EVICT_TEST evicted status=00000000 residency=[23]').Count
 $restores=[regex]::Matches($trace,'EVICT_TEST complete status=00000000 residency=1').Count
 $triggers=[regex]::Matches($result,'EVICT_ALIAS after_producer=1').Count
 "eviction_witnesses=$departures restored=$restores expected=$expected"
 if($departures -ne $expected -or $restores -ne $expected -or $triggers -ne $expected){throw 'Eviction witnesses incomplete'}
 Get-Content "$out\$name.out" -Tail 3
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-ShaderAliasEviction119 -Confirm:$false
'memory_types_run_complete'

'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
