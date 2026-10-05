$ErrorActionPreference='Stop'
$out='C:\BC250\m9\shader-eviction07105'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item -LiteralPath 'C:\BC250\m9\shader-eviction07100\shader-coherency-probe.exe' -Destination "$out\shader-coherency-probe.exe"
$probeHash=(Get-FileHash "$out\shader-coherency-probe.exe").Hash
'probe_hash='+$probeHash
if($probeHash -ne '038010EB3E6220A12F66713BEC9B9DCC527646DA1AAE5E7D3880AADA2F467D9A'){throw 'Probe mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Device not healthy'}
$ver=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$ver
if($ver -ne '0.7.105.1'){throw 'Unexpected driver version'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
$info
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070069' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Actual loaded full driver not confirmed'}
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
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uLWljZFxyYWRlb25faWNkLmpzb24Kc2V0IFZLX0lDRF9GSUxFTkFNRVM9JVZLX0RSSVZFUl9GSUxFUyUKc2V0IFZLX0xPQURFUl9ERUJVRz1kcml2ZXIKc2V0IE1FU0FfU0hBREVSX0NBQ0hFX0RJU0FCTEU9dHJ1ZQpzZXQgQkMyNTBfVFJBQ0VfU1VCTUlUUz0wCnNldCBCQzI1MF9JQl9EV09SRFM9CnNldCBCQzI1MF9URVNUX0VWSUNUX09OX1VOTUFQPQpzZXQgUEFUSD1DOlxCQzI1MFxtODslUEFUSCUKQzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uMDcxMDVcc2hhZGVyLWNvaGVyZW5jeS1wcm9iZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tbWVtb3J5LXR5cGUgMyA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1ldmljdGlvbjA3MTA1XHR5cGUzLXBvc2l0aXZlLm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItZXZpY3Rpb24wNzEwNVx0eXBlMy1wb3NpdGl2ZS5lcnIKc2V0IHByb2JlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJXByb2JlX2V4aXQlID4gQzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uMDcxMDVcdHlwZTMtcG9zaXRpdmUuZXhpdAppZiBub3QgIiVwcm9iZV9leGl0JSI9PSIwIiBleGl0IC9iIDEKQzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uMDcxMDVcc2hhZGVyLWNvaGVyZW5jeS1wcm9iZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tbWVtb3J5LXR5cGUgMyAtLWV2aWN0LWlucHV0IDwgTlVMID4gQzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uMDcxMDVcdHlwZTMtZXZpY3Qub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1ldmljdGlvbjA3MTA1XHR5cGUzLWV2aWN0LmVycgpzZXQgcHJvYmVfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlcHJvYmVfZXhpdCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItZXZpY3Rpb24wNzEwNVx0eXBlMy1ldmljdC5leGl0CmlmIG5vdCAiJXByb2JlX2V4aXQlIj09IjAiIGV4aXQgL2IgMQpDOlxCQzI1MFxtOVxzaGFkZXItZXZpY3Rpb24wNzEwNVxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSAzIC0tZXZpY3QtaW5wdXQgLS1zdGFsZS1pbnB1dC1jb250cm9sIDwgTlVMID4gQzpcQkMyNTBcbTlcc2hhZGVyLWV2aWN0aW9uMDcxMDVcdHlwZTMtc3RhbGUub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1ldmljdGlvbjA3MTA1XHR5cGUzLXN0YWxlLmVycgpzZXQgcHJvYmVfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlcHJvYmVfZXhpdCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItZXZpY3Rpb24wNzEwNVx0eXBlMy1zdGFsZS5leGl0CmlmIG5vdCAiJXByb2JlX2V4aXQlIj09IjEiIGV4aXQgL2IgMQpleGl0IC9iIDAK')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-ShaderEviction07105 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-ShaderEviction07105
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-ShaderEviction07105
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-ShaderEviction07105).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}

foreach($name in @('type3-positive','type3-evict','type3-stale')) {
 $code=[int](Get-Content "$out\$name.exit")
 "$name exit=$code"
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'shader-eviction-icd\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){throw 'Missing actual ICD witness'}
 $result=Get-Content "$out\$name.out" -Raw
 if($name -like '*-stale') {
  if($code -ne 1 -or $result -notmatch 'COHERENCY completed=1 mismatches=1'){throw 'Negative control failed'}
 } else {
  if($code -ne 0 -or $result -notmatch 'COHERENCY completed=16 mismatches=0'){throw 'Positive test failed'}
 }
 if($name -ne 'type3-positive') {
  $expected=if($name -eq 'type3-stale'){1}else{3}
  $departures=[regex]::Matches($trace,'EVICT_TEST evicted status=00000000 residency=[23]').Count
  $restores=[regex]::Matches($trace,'EVICT_TEST complete status=00000000 residency=1').Count
  "eviction_witnesses=$departures restored=$restores expected=$expected"
  if($departures -ne $expected -or $restores -ne $expected){throw 'Missing eviction/restoration witnesses'}
 }
 Get-Content "$out\$name.out" -Tail 3
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-ShaderEviction07105 -Confirm:$false
'memory_types_run_complete'

'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
