$ErrorActionPreference='Stop'
$out='C:\BC250\m9\repeat0798'
if(Test-Path $out){throw 'Output already exists'}
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
New-Item -ItemType Directory -Force $out | Out-Null
$hash=(Get-FileHash C:\BC250\m9\quiet-submit\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '4E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlccXVpZXQtc3VibWl0XHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWNvbXBsZXRpb24uZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1xzdG9yaWVzMTVNLXE0XzAuZ2d1ZiAtcCAiT25jZSB1cG9uIGEgdGltZSIgLW4gOTYgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFxzdG9yaWVzMTVNLTEub3V0IDI+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcc3RvcmllczE1TS0xLmVycgpzZXQgcmVzdWx0PSVFUlJPUkxFVkVMJQplY2hvICVyZXN1bHQlID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFxzdG9yaWVzMTVNLTEuZXhpdAppZiBub3QgIiVyZXN1bHQlIj09IjAiIGV4aXQgL2IgJXJlc3VsdCUKQzpcQkMyNTBcbTlcbGxhbWFcbGxhbWEtY29tcGxldGlvbi5leGUgLW0gQzpcQkMyNTBcbTlcbW9kZWxzXHRpbnlsbGFtYS0xLjFiLWNoYXQtdjEuMC5RNF8wLmdndWYgLXAgIlRoZSBjYXBpdGFsIG9mIEZyYW5jZSBpcyIgLW4gNjQgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFx0aW55bGxhbWEtMS5vdXQgMj4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFx0aW55bGxhbWEtMS5lcnIKc2V0IHJlc3VsdD0lRVJST1JMRVZFTCUKZWNobyAlcmVzdWx0JSA+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcdGlueWxsYW1hLTEuZXhpdAppZiBub3QgIiVyZXN1bHQlIj09IjAiIGV4aXQgL2IgJXJlc3VsdCUKQzpcQkMyNTBcbTlcbGxhbWFcbGxhbWEtY29tcGxldGlvbi5leGUgLW0gQzpcQkMyNTBcbTlcbW9kZWxzXHN0b3JpZXMxNU0tcTRfMC5nZ3VmIC1wICJPbmNlIHVwb24gYSB0aW1lIiAtbiA5NiAtLXRlbXAgMCAtLXNlZWQgMSAtbmdsIDk5IC1uby1jbnYgLXQgNiAtLXZlcmJvc2UgPCBOVUwgPiBDOlxCQzI1MFxtOVxyZXBlYXQwNzk4XHN0b3JpZXMxNU0tMi5vdXQgMj4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFxzdG9yaWVzMTVNLTIuZXJyCnNldCByZXN1bHQ9JUVSUk9STEVWRUwlCmVjaG8gJXJlc3VsdCUgPiBDOlxCQzI1MFxtOVxyZXBlYXQwNzk4XHN0b3JpZXMxNU0tMi5leGl0CmlmIG5vdCAiJXJlc3VsdCUiPT0iMCIgZXhpdCAvYiAlcmVzdWx0JQpDOlxCQzI1MFxtOVxsbGFtYVxsbGFtYS1jb21wbGV0aW9uLmV4ZSAtbSBDOlxCQzI1MFxtOVxtb2RlbHNcdGlueWxsYW1hLTEuMWItY2hhdC12MS4wLlE0XzAuZ2d1ZiAtcCAiVGhlIGNhcGl0YWwgb2YgRnJhbmNlIGlzIiAtbiA2NCAtLXRlbXAgMCAtLXNlZWQgMSAtbmdsIDk5IC1uby1jbnYgLXQgNiAtLXZlcmJvc2UgPCBOVUwgPiBDOlxCQzI1MFxtOVxyZXBlYXQwNzk4XHRpbnlsbGFtYS0yLm91dCAyPiBDOlxCQzI1MFxtOVxyZXBlYXQwNzk4XHRpbnlsbGFtYS0yLmVycgpzZXQgcmVzdWx0PSVFUlJPUkxFVkVMJQplY2hvICVyZXN1bHQlID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFx0aW55bGxhbWEtMi5leGl0CmlmIG5vdCAiJXJlc3VsdCUiPT0iMCIgZXhpdCAvYiAlcmVzdWx0JQpDOlxCQzI1MFxtOVxsbGFtYVxsbGFtYS1jb21wbGV0aW9uLmV4ZSAtbSBDOlxCQzI1MFxtOVxtb2RlbHNcc3RvcmllczE1TS1xNF8wLmdndWYgLXAgIk9uY2UgdXBvbiBhIHRpbWUiIC1uIDk2IC0tdGVtcCAwIC0tc2VlZCAxIC1uZ2wgOTkgLW5vLWNudiAtdCA2IC0tdmVyYm9zZSA8IE5VTCA+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcc3RvcmllczE1TS0zLm91dCAyPiBDOlxCQzI1MFxtOVxyZXBlYXQwNzk4XHN0b3JpZXMxNU0tMy5lcnIKc2V0IHJlc3VsdD0lRVJST1JMRVZFTCUKZWNobyAlcmVzdWx0JSA+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcc3RvcmllczE1TS0zLmV4aXQKaWYgbm90ICIlcmVzdWx0JSI9PSIwIiBleGl0IC9iICVyZXN1bHQlCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWNvbXBsZXRpb24uZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1x0aW55bGxhbWEtMS4xYi1jaGF0LXYxLjAuUTRfMC5nZ3VmIC1wICJUaGUgY2FwaXRhbCBvZiBGcmFuY2UgaXMiIC1uIDY0IC0tdGVtcCAwIC0tc2VlZCAxIC1uZ2wgOTkgLW5vLWNudiAtdCA2IC0tdmVyYm9zZSA8IE5VTCA+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcdGlueWxsYW1hLTMub3V0IDI+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcdGlueWxsYW1hLTMuZXJyCnNldCByZXN1bHQ9JUVSUk9STEVWRUwlCmVjaG8gJXJlc3VsdCUgPiBDOlxCQzI1MFxtOVxyZXBlYXQwNzk4XHRpbnlsbGFtYS0zLmV4aXQKaWYgbm90ICIlcmVzdWx0JSI9PSIwIiBleGl0IC9iICVyZXN1bHQlCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWNvbXBsZXRpb24uZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1xzdG9yaWVzMTVNLXE0XzAuZ2d1ZiAtcCAiT25jZSB1cG9uIGEgdGltZSIgLW4gOTYgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFxzdG9yaWVzMTVNLTQub3V0IDI+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcc3RvcmllczE1TS00LmVycgpzZXQgcmVzdWx0PSVFUlJPUkxFVkVMJQplY2hvICVyZXN1bHQlID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFxzdG9yaWVzMTVNLTQuZXhpdAppZiBub3QgIiVyZXN1bHQlIj09IjAiIGV4aXQgL2IgJXJlc3VsdCUKQzpcQkMyNTBcbTlcbGxhbWFcbGxhbWEtY29tcGxldGlvbi5leGUgLW0gQzpcQkMyNTBcbTlcbW9kZWxzXHRpbnlsbGFtYS0xLjFiLWNoYXQtdjEuMC5RNF8wLmdndWYgLXAgIlRoZSBjYXBpdGFsIG9mIEZyYW5jZSBpcyIgLW4gNjQgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFx0aW55bGxhbWEtNC5vdXQgMj4gQzpcQkMyNTBcbTlccmVwZWF0MDc5OFx0aW55bGxhbWEtNC5lcnIKc2V0IHJlc3VsdD0lRVJST1JMRVZFTCUKZWNobyAlcmVzdWx0JSA+IEM6XEJDMjUwXG05XHJlcGVhdDA3OThcdGlueWxsYW1hLTQuZXhpdAppZiBub3QgIiVyZXN1bHQlIj09IjAiIGV4aXQgL2IgJXJlc3VsdCUKZXhpdCAvYiAwCg==')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
$action=New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -Command Start-Process cmd.exe -ArgumentList '/c $out\run.cmd' -WindowStyle Hidden -Wait"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-Repeat0798 -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Repeat0798
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-Repeat0798
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-Repeat0798).State
}while($state -eq 'Running' -and (Get-Date) -lt $deadline)
'task_state='+$state
if($state -eq 'Running'){throw 'Task still running, inspect before another run'}
foreach($name in @('stories15M-1','tinyllama-1','stories15M-2','tinyllama-2','stories15M-3','tinyllama-3','stories15M-4','tinyllama-4')) {
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
  $model=$name -replace '-[1-4]$',''
 $expected=[IO.File]::ReadAllText("C:\BC250\m9\reference\$model-ngl99.out").Replace([string][char]13,'')
  $actual=[IO.File]::ReadAllText("$out\$name.out").Replace([string][char]13,'')
  if($actual -ne $expected){throw "Reference mismatch $name"}
  "$name reference_match"
 }
 Get-Content "$out\$name.out" -Tail 4
}
& C:\BC250\m8\bc250kmd_cli.exe log summary
Unregister-ScheduledTask BC250-M9-Repeat0798 -Confirm:$false
'limited_run_complete'
