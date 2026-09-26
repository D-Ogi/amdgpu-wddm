$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) { throw 'Owner STOP requested' }
$out='C:\BC250\m9\cold145-control'
if(Test-Path $out){throw "Output already exists"}
New-Item -ItemType Directory $out | Out-Null
& C:\BC250\bc250rd\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
$gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'Adapter unhealthy'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
if($version -ne '0.7.145.1'){throw 'Unexpected KMD'}
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x00070091' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded driver mismatch'}
$image=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($image.StartsWith('\SystemRoot\')){$image=Join-Path $env:windir $image.Substring(12)}
if($image.StartsWith('\??\')){$image=$image.Substring(4)}
$sha=(Get-FileHash -LiteralPath $image).Hash
'installed_sha256='+$sha
if($sha -ne 'ED7B2E0735A41048DDA1428FB4A759C32193A231D5A26A5C2FEAD5D8407903CB'){throw 'SYS mismatch'}
foreach($file in @('C:\BC250\m9\llama\llama-completion.exe','C:\BC250\m9\models\stories15M-q4_0.gguf','C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf')) {
 'input_file='+[IO.Path]::GetFileName($file)+' sha256='+(Get-FileHash -LiteralPath $file).Hash
}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"

$hash=(Get-FileHash C:\BC250\m9\radv-main-icd2\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne 'DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlccmFkdi1tYWluLWljZDJccmFkZW9uX2ljZC5qc29uCnNldCBWS19JQ0RfRklMRU5BTUVTPSVWS19EUklWRVJfRklMRVMlCnNldCBWS19MT0FERVJfREVCVUc9ZHJpdmVyCnNldCBNRVNBX1NIQURFUl9DQUNIRV9ESVNBQkxFPXRydWUKc2V0IEJDMjUwX1RSQUNFX1NVQk1JVFM9MApzZXQgQkMyNTBfSUJfRFdPUkRTPQpzZXQgUEFUSD1DOlxCQzI1MFxtODslUEFUSCUKQzpcQkMyNTBcbThcdmtjb21wdXRlLmV4ZSBDOlxCQzI1MFxtOFxzcHYgLS1ydW5zIDMgPCBOVUwgPiBDOlxCQzI1MFxtOVxjb2xkMTQ1LWNvbnRyb2xcbTgub3V0IDI+IEM6XEJDMjUwXG05XGNvbGQxNDUtY29udHJvbFxtOC5lcnIKc2V0IGluZmVyZW5jZV9leGl0PSVFUlJPUkxFVkVMJQplY2hvICVpbmZlcmVuY2VfZXhpdCUgPiBDOlxCQzI1MFxtOVxjb2xkMTQ1LWNvbnRyb2xcbTguZXhpdAppZiBub3QgIiVpbmZlcmVuY2VfZXhpdCUiPT0iMCIgZXhpdCAvYiAxCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWNvbXBsZXRpb24uZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1xzdG9yaWVzMTVNLXE0XzAuZ2d1ZiAtcCAiT25jZSB1cG9uIGEgdGltZSIgLW4gOTYgLS10ZW1wIDAgLS1zZWVkIDEgLW5nbCA5OSAtbm8tY252IC10IDYgLS12ZXJib3NlIDwgTlVMID4gQzpcQkMyNTBcbTlcY29sZDE0NS1jb250cm9sXHN0b3JpZXMxNU0ub3V0IDI+IEM6XEJDMjUwXG05XGNvbGQxNDUtY29udHJvbFxzdG9yaWVzMTVNLmVycgpzZXQgaW5mZXJlbmNlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJWluZmVyZW5jZV9leGl0JSA+IEM6XEJDMjUwXG05XGNvbGQxNDUtY29udHJvbFxzdG9yaWVzMTVNLmV4aXQKaWYgbm90ICIlaW5mZXJlbmNlX2V4aXQlIj09IjAiIGV4aXQgL2IgMQpDOlxCQzI1MFxtOVxsbGFtYVxsbGFtYS1jb21wbGV0aW9uLmV4ZSAtbSBDOlxCQzI1MFxtOVxtb2RlbHNcdGlueWxsYW1hLTEuMWItY2hhdC12MS4wLlE0XzAuZ2d1ZiAtcCAiVGhlIGNhcGl0YWwgb2YgRnJhbmNlIGlzIiAtbiA2NCAtLXRlbXAgMCAtLXNlZWQgMSAtbmdsIDk5IC1uby1jbnYgLXQgNiAtLXZlcmJvc2UgPCBOVUwgPiBDOlxCQzI1MFxtOVxjb2xkMTQ1LWNvbnRyb2xcdGlueWxsYW1hLm91dCAyPiBDOlxCQzI1MFxtOVxjb2xkMTQ1LWNvbnRyb2xcdGlueWxsYW1hLmVycgpzZXQgaW5mZXJlbmNlX2V4aXQ9JUVSUk9STEVWRUwlCmVjaG8gJWluZmVyZW5jZV9leGl0JSA+IEM6XEJDMjUwXG05XGNvbGQxNDUtY29udHJvbFx0aW55bGxhbWEuZXhpdApleGl0IC9iICVpbmZlcmVuY2VfZXhpdCUK')) | Set-Content "$out\run.cmd" -Encoding ASCII
$user=(Get-CimInstance Win32_ComputerSystem).UserName
if(-not $user){throw 'No interactive user'}
@"
& cmd.exe /c $out\run.cmd
`$nativeExit=`$LASTEXITCODE
[IO.File]::WriteAllText('$out\worker.exit',[string]`$nativeExit)
exit `$nativeExit
"@ | Set-Content "$out\worker.ps1" -Encoding ASCII
$action=New-ScheduledTaskAction -Execute conhost.exe -Argument "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File $out\worker.ps1"
$principal=New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName BC250-M9-Cold145Control -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-Cold145Control
$deadline=(Get-Date).AddSeconds(300)
do {
 if ((Invoke-RestMethod http://127.0.0.1:2250/flags).stop) { Stop-ScheduledTask BC250-M9-Cold145Control; throw 'Owner STOP requested' }
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-Cold145Control
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-Cold145Control).State
}while(-not (Test-Path "$out\worker.exit") -and (Get-Date) -lt $deadline)
'task_state='+$state
if(-not (Test-Path "$out\worker.exit")){Stop-ScheduledTask BC250-M9-Cold145Control; throw 'Native worker deadline; inspect before another run'}
'worker_exit='+(Get-Content "$out\worker.exit")
foreach($name in @('m8','stories15M','tinyllama')) {
 $exit=[int](Get-Content "$out\$name.exit")
 "$name exit=$exit"
 if($exit -ne 0){throw "Failed $name"}
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'radv-main-icd2\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){
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
# Auto-confirmation remains the startup witness; do not manually reset it here.
Get-Process dwm | Select-Object Id,StartTime,Responding
'final_time='+(Get-Date).ToString('s')
Unregister-ScheduledTask BC250-M9-Cold145Control -Confirm:$false
'limited_run_complete'

'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
