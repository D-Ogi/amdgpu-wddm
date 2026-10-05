$ErrorActionPreference='Stop'
$out='C:\BC250\m9\bench0798'
if(Test-Path $out){throw 'Output already exists'}
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
New-Item -ItemType Directory -Force $out | Out-Null
$hash=(Get-FileHash C:\BC250\m9\quiet-submit\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '4E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlccXVpZXQtc3VibWl0XHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWJlbmNoLmV4ZSAtbSBDOlxCQzI1MFxtOVxtb2RlbHNcc3RvcmllczE1TS1xNF8wLmdndWYgLW5nbCA5OSAtcCA1MTIgLW4gMTI4IC1yIDMgLXQgNiAtbyBqc29uIC12IDwgTlVMID4gQzpcQkMyNTBcbTlcYmVuY2gwNzk4XHN0b3JpZXMxNU0ub3V0IDI+IEM6XEJDMjUwXG05XGJlbmNoMDc5OFxzdG9yaWVzMTVNLmVycgpzZXQgcmVzdWx0PSVFUlJPUkxFVkVMJQplY2hvICVyZXN1bHQlID4gQzpcQkMyNTBcbTlcYmVuY2gwNzk4XHN0b3JpZXMxNU0uZXhpdAppZiBub3QgIiVyZXN1bHQlIj09IjAiIGV4aXQgL2IgJXJlc3VsdCUKQzpcQkMyNTBcbTlcbGxhbWFcbGxhbWEtYmVuY2guZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1x0aW55bGxhbWEtMS4xYi1jaGF0LXYxLjAuUTRfMC5nZ3VmIC1uZ2wgOTkgLXAgNTEyIC1uIDEyOCAtciAzIC10IDYgLW8ganNvbiAtdiA8IE5VTCA+IEM6XEJDMjUwXG05XGJlbmNoMDc5OFx0aW55bGxhbWEub3V0IDI+IEM6XEJDMjUwXG05XGJlbmNoMDc5OFx0aW55bGxhbWEuZXJyCnNldCByZXN1bHQ9JUVSUk9STEVWRUwlCmVjaG8gJXJlc3VsdCUgPiBDOlxCQzI1MFxtOVxiZW5jaDA3OThcdGlueWxsYW1hLmV4aXQKaWYgbm90ICIlcmVzdWx0JSI9PSIwIiBleGl0IC9iICVyZXN1bHQlCmV4aXQgL2IgMAo=')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-Bench0798 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Bench0798
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-Bench0798
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-Bench0798).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}
foreach($name in @('stories15M','tinyllama')) {
 $exit=[int](Get-Content "$out\$name.exit")
 "$name exit=$exit"
 if($exit -ne 0){throw "Failed $name"}
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'quiet-submit\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){
  throw "Missing loaded ICD witness: $name"
 }
 Get-Content "$out\$name.out" -Tail 4
}
& C:\BC250\m8\bc250kmd_cli.exe log summary
Unregister-ScheduledTask BC250-M9-Bench0798 -Confirm:$false
'limited_run_complete'
