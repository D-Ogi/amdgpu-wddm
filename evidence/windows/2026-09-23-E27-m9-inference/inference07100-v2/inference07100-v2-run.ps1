$ErrorActionPreference='Stop'
$out='C:\BC250\m9\inference07100-v2'
New-Item -ItemType Directory -Force $out | Out-Null
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Adapter unhealthy'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if($version -ne '0.7.100.1'){throw 'Unexpected KMD'}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"

$hash=(Get-FileHash C:\BC250\m9\cache-intent-v2\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlcY2FjaGUtaW50ZW50LXYyXHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG04XHZrY29tcHV0ZS5leGUgQzpcQkMyNTBcbThcc3B2IC0tcnVucyAzIDwgTlVMID4gQzpcQkMyNTBcbTlcaW5mZXJlbmNlMDcxMDAtdjJcbTgub3V0IDI+IEM6XEJDMjUwXG05XGluZmVyZW5jZTA3MTAwLXYyXG04LmVycgpzZXQgaW5mZXJlbmNlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJWluZmVyZW5jZV9leGl0JSA+IEM6XEJDMjUwXG05XGluZmVyZW5jZTA3MTAwLXYyXG04LmV4aXQKaWYgbm90ICIlaW5mZXJlbmNlX2V4aXQlIj09IjAiIGV4aXQgL2IgMQpDOlxCQzI1MFxtOVxsbGFtYVxsbGFtYS1jb21wbGV0aW9uLmV4ZSAtbSBDOlxCQzI1MFxtOVxtb2RlbHNcc3RvcmllczE1TS1xNF8wLmdndWYgLXAgIk9uY2UgdXBvbiBhIHRpbWUiIC1uIDk2IC0tdGVtcCAwIC0tc2VlZCAxIC1uZ2wgOTkgLW5vLWNudiAtdCA2IC0tdmVyYm9zZSA8IE5VTCA+IEM6XEJDMjUwXG05XGluZmVyZW5jZTA3MTAwLXYyXHN0b3JpZXMxNU0ub3V0IDI+IEM6XEJDMjUwXG05XGluZmVyZW5jZTA3MTAwLXYyXHN0b3JpZXMxNU0uZXJyCnNldCBpbmZlcmVuY2VfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlaW5mZXJlbmNlX2V4aXQlID4gQzpcQkMyNTBcbTlcaW5mZXJlbmNlMDcxMDAtdjJcc3RvcmllczE1TS5leGl0CmlmIG5vdCAiJWluZmVyZW5jZV9leGl0JSI9PSIwIiBleGl0IC9iIDEKQzpcQkMyNTBcbTlcbGxhbWFcbGxhbWEtY29tcGxldGlvbi5leGUgLW0gQzpcQkMyNTBcbTlcbW9kZWxzXHRpbnlsbGFtYS0xLjFiLWNoYXQtdjEuMC5RNF8wLmdndWYgLXAgIlRoZSBjYXBpdGFsIG9mIEZyYW5jZSBpcyIgLW4gNjQgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlcaW5mZXJlbmNlMDcxMDAtdjJcdGlueWxsYW1hLm91dCAyPiBDOlxCQzI1MFxtOVxpbmZlcmVuY2UwNzEwMC12Mlx0aW55bGxhbWEuZXJyCnNldCBpbmZlcmVuY2VfZXhpdD0lRVJST1JMRVZFTCUKZWNobyAlaW5mZXJlbmNlX2V4aXQlID4gQzpcQkMyNTBcbTlcaW5mZXJlbmNlMDcxMDAtdjJcdGlueWxsYW1hLmV4aXQKZXhpdCAvYiAlaW5mZXJlbmNlX2V4aXQlCg==')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-Inference07100V2 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Inference07100V2
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-Inference07100V2
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-Inference07100V2).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}
foreach($name in @('m8','stories15M','tinyllama')) {
 $exit=[int](Get-Content "$out\$name.exit")
 "$name exit=$exit"
 if($exit -ne 0){throw "Failed $name"}
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'cache-intent-v2\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){
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
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-Inference07100V2 -Confirm:$false
'limited_run_complete'

'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
