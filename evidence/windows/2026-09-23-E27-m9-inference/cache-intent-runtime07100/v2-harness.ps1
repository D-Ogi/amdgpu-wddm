$ErrorActionPreference='Stop'
$out='C:\BC250\m9\shader-memory-types07100-v2'
New-Item -ItemType Directory -Force $out | Out-Null
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"
$hash=(Get-FileHash C:\BC250\m9\cache-intent-v2\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlcY2FjaGUtaW50ZW50LXYyXHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12MlxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSAyIDwgTlVMID4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUyLXBvc2l0aXZlLm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTItcG9zaXRpdmUuZXJyCnNldCBwcm9iZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVwcm9iZV9leGl0JSA+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMi1wb3NpdGl2ZS5leGl0CmlmIG5vdCAiJXByb2JlX2V4aXQlIj09IjAiIGV4aXQgL2IgMQpDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcc2hhZGVyLWNvaGVyZW5jeS1wcm9iZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tbWVtb3J5LXR5cGUgMiAtLXN0YWxlLWlucHV0LWNvbnRyb2wgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTItc3RhbGUub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMi1zdGFsZS5lcnIKc2V0IHByb2JlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJXByb2JlX2V4aXQlID4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUyLXN0YWxlLmV4aXQKaWYgbm90ICIlcHJvYmVfZXhpdCUiPT0iMSIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12MlxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSAyIDwgTlVMID4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUyLXJlcGVhdC5vdXQgMj4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUyLXJlcGVhdC5lcnIKc2V0IHByb2JlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJXByb2JlX2V4aXQlID4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUyLXJlcGVhdC5leGl0CmlmIG5vdCAiJXByb2JlX2V4aXQlIj09IjAiIGV4aXQgL2IgMQpDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcc2hhZGVyLWNvaGVyZW5jeS1wcm9iZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tbWVtb3J5LXR5cGUgMyA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMy1wb3NpdGl2ZS5vdXQgMj4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUzLXBvc2l0aXZlLmVycgpzZXQgcHJvYmVfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlcHJvYmVfZXhpdCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTMtcG9zaXRpdmUuZXhpdAppZiBub3QgIiVwcm9iZV9leGl0JSI9PSIwIiBleGl0IC9iIDEKQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiAtLW1lbW9yeS10eXBlIDMgLS1zdGFsZS1pbnB1dC1jb250cm9sIDwgTlVMID4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGUzLXN0YWxlLm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTMtc3RhbGUuZXJyCnNldCBwcm9iZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVwcm9iZV9leGl0JSA+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMy1zdGFsZS5leGl0CmlmIG5vdCAiJXByb2JlX2V4aXQlIj09IjEiIGV4aXQgL2IgMQpDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcc2hhZGVyLWNvaGVyZW5jeS1wcm9iZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tbWVtb3J5LXR5cGUgMyA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMy1yZXBlYXQub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMy1yZXBlYXQuZXJyCnNldCBwcm9iZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVwcm9iZV9leGl0JSA+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlMy1yZXBlYXQuZXhpdAppZiBub3QgIiVwcm9iZV9leGl0JSI9PSIwIiBleGl0IC9iIDEKQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiAtLW1lbW9yeS10eXBlIDUgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTUtcG9zaXRpdmUub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlNS1wb3NpdGl2ZS5lcnIKc2V0IHByb2JlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJXByb2JlX2V4aXQlID4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGU1LXBvc2l0aXZlLmV4aXQKaWYgbm90ICIlcHJvYmVfZXhpdCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12MlxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1tZW1vcnktdHlwZSA1IC0tc3RhbGUtaW5wdXQtY29udHJvbCA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1tZW1vcnktdHlwZXMwNzEwMC12Mlx0eXBlNS1zdGFsZS5vdXQgMj4gQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHR5cGU1LXN0YWxlLmVycgpzZXQgcHJvYmVfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlcHJvYmVfZXhpdCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTUtc3RhbGUuZXhpdAppZiBub3QgIiVwcm9iZV9leGl0JSI9PSIxIiBleGl0IC9iIDEKQzpcQkMyNTBcbTlcc2hhZGVyLW1lbW9yeS10eXBlczA3MTAwLXYyXHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiAtLW1lbW9yeS10eXBlIDUgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTUtcmVwZWF0Lm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTUtcmVwZWF0LmVycgpzZXQgcHJvYmVfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlcHJvYmVfZXhpdCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItbWVtb3J5LXR5cGVzMDcxMDAtdjJcdHlwZTUtcmVwZWF0LmV4aXQKaWYgbm90ICIlcHJvYmVfZXhpdCUiPT0iMCIgZXhpdCAvYiAxCmV4aXQgL2IgMAo=')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-MemoryTypes07100V2 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-MemoryTypes07100V2
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-MemoryTypes07100V2
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-MemoryTypes07100V2).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}

foreach($name in @('type2-positive','type2-stale','type2-repeat','type3-positive','type3-stale','type3-repeat','type5-positive','type5-stale','type5-repeat')) {
 $code=[int](Get-Content "$out\$name.exit")
 "$name exit=$code"
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'cache-intent-v2\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){throw 'Missing actual ICD witness'}
 $result=Get-Content "$out\$name.out" -Raw
 if($name -like '*-stale') {
  if($code -ne 1 -or $result -notmatch 'COHERENCY completed=1 mismatches=1'){throw 'Negative control failed'}
 } else {
  if($code -ne 0 -or $result -notmatch 'COHERENCY completed=16 mismatches=0'){throw 'Positive test failed'}
 }
 Get-Content "$out\$name.out" -Tail 3
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-MemoryTypes07100V2 -Confirm:$false
'memory_types_run_complete'
