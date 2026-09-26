$ErrorActionPreference='Stop'
$out='C:\BC250\m9\limited0798'
New-Item -ItemType Directory -Force $out | Out-Null
$hash=(Get-FileHash C:\BC250\m9\quiet-submit\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '4E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlccXVpZXQtc3VibWl0XHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG04XHZrY29tcHV0ZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tcnVucyAzIDwgTlVMID4gQzpcQkMyNTBcbTlcbGltaXRlZDA3OThcbTgub3V0IDI+IEM6XEJDMjUwXG05XGxpbWl0ZWQwNzk4XG04LmVycgplY2hvICVFUlJPUkxFVkVMJSA+IEM6XEJDMjUwXG05XGxpbWl0ZWQwNzk4XG04LmV4aXQKaWYgbm90ICIlRVJST1JMRVZFTCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWNvbXBsZXRpb24uZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1xzdG9yaWVzMTVNLXE0XzAuZ2d1ZiAtcCAiT25jZSB1cG9uIGEgdGltZSIgLW4gOTYgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlcbGltaXRlZDA3OThcc3RvcmllczE1TS5vdXQgMj4gQzpcQkMyNTBcbTlcbGltaXRlZDA3OThcc3RvcmllczE1TS5lcnIKZWNobyAlRVJST1JMRVZFTCUgPiBDOlxCQzI1MFxtOVxsaW1pdGVkMDc5OFxzdG9yaWVzMTVNLmV4aXQKaWYgbm90ICIlRVJST1JMRVZFTCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWNvbXBsZXRpb24uZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1x0aW55bGxhbWEtMS4xYi1jaGF0LXYxLjAuUTRfMC5nZ3VmIC1wICJUaGUgY2FwaXRhbCBvZiBGcmFuY2UgaXMiIC1uIDY0IC0tdGVtcCAwIC0tc2VlZCAxIC1uZ2wgOTkgLW5vLWNudiAtdCA2IC0tdmVyYm9zZSA8IE5VTCA+IEM6XEJDMjUwXG05XGxpbWl0ZWQwNzk4XHRpbnlsbGFtYS5vdXQgMj4gQzpcQkMyNTBcbTlcbGltaXRlZDA3OThcdGlueWxsYW1hLmVycgplY2hvICVFUlJPUkxFVkVMJSA+IEM6XEJDMjUwXG05XGxpbWl0ZWQwNzk4XHRpbnlsbGFtYS5leGl0CmV4aXQgL2IK')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-Limited0798 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Limited0798
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-Limited0798
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-Limited0798).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}
foreach($name in @('m8','stories15M','tinyllama')) {
 $exit=[int](Get-Content "$out\$name.exit")
 "$name exit=$exit"
 if($exit -ne 0){throw "Failed $name"}
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'quiet-submit\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){
  throw "Missing loaded ICD witness: $name"
 }
 if($name -ne 'm8'){
  $offload=[regex]::Match($trace,'offloaded ([0-9]+)/([0-9]+) layers to GPU')
  if(-not $offload.Success -or $offload.Groups[1].Value -ne $offload.Groups[2].Value){throw 'Offload mismatch'}
  $expected=[IO.File]::ReadAllText("C:\BC250\m9\reference\$name-ngl99.out").Replace([string][char]13,'')
  $actual=[IO.File]::ReadAllText("$out\$name.out").Replace([string][char]13,'')
  if($actual -ne $expected){throw "Reference mismatch $name"}
  "$name reference_match"
 }
 Get-Content "$out\$name.out" -Tail 4
}
& C:\BC250\m8\bc250kmd_cli.exe log summary
Unregister-ScheduledTask BC250-M9-Limited0798 -Confirm:$false
'limited_run_complete'
