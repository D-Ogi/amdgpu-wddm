$ErrorActionPreference='Stop'
$out='C:\BC250\m9\shader-coherency0798'
New-Item -ItemType Directory -Force $out | Out-Null
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"
$hash=(Get-FileHash C:\BC250\m9\quiet-submit\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '4E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlccXVpZXQtc3VibWl0XHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG05XHNoYWRlci1jb2hlcmVuY3kwNzk4XHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiA8IE5VTCA+IEM6XEJDMjUwXG05XHNoYWRlci1jb2hlcmVuY3kwNzk4XHBvc2l0aXZlLm91dCAyPiBDOlxCQzI1MFxtOVxzaGFkZXItY29oZXJlbmN5MDc5OFxwb3NpdGl2ZS5lcnIKZWNobyAlRVJST1JMRVZFTCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItY29oZXJlbmN5MDc5OFxwb3NpdGl2ZS5leGl0CkM6XEJDMjUwXG05XHNoYWRlci1jb2hlcmVuY3kwNzk4XHNoYWRlci1jb2hlcmVuY3ktcHJvYmUuZXhlIEM6XEJDMjUwXG04XHNwdiAtLXN0YWxlLWlucHV0LWNvbnRyb2wgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItY29oZXJlbmN5MDc5OFxzdGFsZS5vdXQgMj4gQzpcQkMyNTBcbTlcc2hhZGVyLWNvaGVyZW5jeTA3OThcc3RhbGUuZXJyCmVjaG8gJUVSUk9STEVWRUwlID4gQzpcQkMyNTBcbTlcc2hhZGVyLWNvaGVyZW5jeTA3OThcc3RhbGUuZXhpdApDOlxCQzI1MFxtOVxzaGFkZXItY29oZXJlbmN5MDc5OFxzaGFkZXItY29oZXJlbmN5LXByb2JlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgPCBOVUwgPiBDOlxCQzI1MFxtOVxzaGFkZXItY29oZXJlbmN5MDc5OFxyZXBlYXQub3V0IDI+IEM6XEJDMjUwXG05XHNoYWRlci1jb2hlcmVuY3kwNzk4XHJlcGVhdC5lcnIKZWNobyAlRVJST1JMRVZFTCUgPiBDOlxCQzI1MFxtOVxzaGFkZXItY29oZXJlbmN5MDc5OFxyZXBlYXQuZXhpdApleGl0IC9iIDAK')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-Coherency0798 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Coherency0798
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-Coherency0798
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-Coherency0798).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}

foreach($name in @('positive','stale','repeat')) {
 $code=[int](Get-Content "$out\$name.exit")
 "$name exit=$code"
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'quiet-submit\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){throw 'Missing actual ICD witness'}
 $result=Get-Content "$out\$name.out" -Raw
 if($name -eq 'stale') {
  if($code -ne 1 -or $result -notmatch 'COHERENCY completed=1 mismatches=1'){throw 'Negative control failed'}
 } else {
  if($code -ne 0 -or $result -notmatch 'COHERENCY completed=16 mismatches=0'){throw 'Positive test failed'}
 }
 Get-Content "$out\$name.out" -Tail 3
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-Coherency0798 -Confirm:$false
'coherency_run_complete'
