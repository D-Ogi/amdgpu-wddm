$ErrorActionPreference='Stop'
if ((Invoke-RestMethod http://127.0.0.1:2250/state).stop) {throw 'Owner STOP requested'}
Get-Process dwm | Select-Object Id,StartTime,CPU | Format-Table
$out='C:\BC250\m9\radv-main-bench-old'
if(Test-Path $out){throw 'Output already exists'}
& C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe clock-check 1000 820
if($LASTEXITCODE -ne 0){throw 'Clock mismatch'}
'boot_before='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
$gpu=@(Get-PnpDevice -Class Display | Where-Object {$_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*'})
if($gpu.Count -ne 1 -or $gpu[0].Status -ne 'OK'){throw 'No healthy adapter'}
$version=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'driver_version='+$version
if($version -ne '0.7.127.1'){throw 'Unexpected driver'}
$temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
$temp.Trim()
if($LASTEXITCODE -ne 0 -or $temp -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'No temperature'}
if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
New-Item -ItemType Directory -Force $out | Out-Null
$info=& C:\BC250\m8\bc250kmd_cli.exe info | Out-String
if($LASTEXITCODE -ne 0 -or $info -notmatch '0x0007007F' -or $info -notmatch 'FULL WDDM TABLE'){throw 'Loaded driver mismatch'}
$path=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd').ImagePath
if($path.StartsWith('\SystemRoot\')){$path=Join-Path $env:windir $path.Substring(12)}
if($path.StartsWith('\??\')){$path=$path.Substring(4)}
$sha=(Get-FileHash -LiteralPath $path).Hash
'installed_sha256='+$sha
if($sha -ne '476858800F28A5C103E43FECCC8595C6042BA2479EE18FCFCC3D9FD313953E90'){throw 'Installed image mismatch'}
foreach($file in @('C:\BC250\m9\llama\llama-bench.exe','C:\BC250\m9\models\stories15M-q4_0.gguf','C:\BC250\m9\models\tinyllama-1.1b-chat-v1.0.Q4_0.gguf')) {
 'input_file='+[IO.Path]::GetFileName($file)+' sha256='+(Get-FileHash -LiteralPath $file).Hash
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\before.log"

$hash=(Get-FileHash C:\BC250\m9\cache-intent-v2\vulkan_radeon.dll).Hash
'icd_hash='+$hash
if($hash -ne '6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754'){throw 'ICD mismatch'}
[Text.Encoding]::ASCII.GetString([Convert]::FromBase64String('QGVjaG8gb2ZmCnNldCBWS19EUklWRVJfRklMRVM9QzpcQkMyNTBcbTlcY2FjaGUtaW50ZW50LXYyXHJhZGVvbl9pY2QuanNvbgpzZXQgVktfSUNEX0ZJTEVOQU1FUz0lVktfRFJJVkVSX0ZJTEVTJQpzZXQgVktfTE9BREVSX0RFQlVHPWRyaXZlcgpzZXQgTUVTQV9TSEFERVJfQ0FDSEVfRElTQUJMRT10cnVlCnNldCBCQzI1MF9UUkFDRV9TVUJNSVRTPTAKc2V0IEJDMjUwX0lCX0RXT1JEUz0Kc2V0IFBBVEg9QzpcQkMyNTBcbTg7JVBBVEglCkM6XEJDMjUwXG05XGxsYW1hXGxsYW1hLWJlbmNoLmV4ZSAtbSBDOlxCQzI1MFxtOVxtb2RlbHNcc3RvcmllczE1TS1xNF8wLmdndWYgLW5nbCA5OSAtcCA1MTIgLW4gMTI4IC1yIDMgLXQgNiAtbyBqc29uIC12IDwgTlVMID4gQzpcQkMyNTBcbTlccmFkdi1tYWluLWJlbmNoLW9sZFxzdG9yaWVzMTVNLm91dCAyPiBDOlxCQzI1MFxtOVxyYWR2LW1haW4tYmVuY2gtb2xkXHN0b3JpZXMxNU0uZXJyCnNldCByZXN1bHQ9JUVSUk9STEVWRUwlCmVjaG8gJXJlc3VsdCUgPiBDOlxCQzI1MFxtOVxyYWR2LW1haW4tYmVuY2gtb2xkXHN0b3JpZXMxNU0uZXhpdAppZiBub3QgIiVyZXN1bHQlIj09IjAiIGV4aXQgL2IgJXJlc3VsdCUKQzpcQkMyNTBcbTlcbGxhbWFcbGxhbWEtYmVuY2guZXhlIC1tIEM6XEJDMjUwXG05XG1vZGVsc1x0aW55bGxhbWEtMS4xYi1jaGF0LXYxLjAuUTRfMC5nZ3VmIC1uZ2wgOTkgLXAgNTEyIC1uIDEyOCAtciAzIC10IDYgLW8ganNvbiAtdiA8IE5VTCA+IEM6XEJDMjUwXG05XHJhZHYtbWFpbi1iZW5jaC1vbGRcdGlueWxsYW1hLm91dCAyPiBDOlxCQzI1MFxtOVxyYWR2LW1haW4tYmVuY2gtb2xkXHRpbnlsbGFtYS5lcnIKc2V0IHJlc3VsdD0lRVJST1JMRVZFTCUKZWNobyAlcmVzdWx0JSA+IEM6XEJDMjUwXG05XHJhZHYtbWFpbi1iZW5jaC1vbGRcdGlueWxsYW1hLmV4aXQKaWYgbm90ICIlcmVzdWx0JSI9PSIwIiBleGl0IC9iICVyZXN1bHQlCmV4aXQgL2IgMAo=')) | Set-Content "$out\run.cmd" -Encoding ASCII
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
Register-ScheduledTask -TaskName BC250-M9-RadvMainBenchOld -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask BC250-M9-RadvMainBenchOld
$deadline=(Get-Date).AddSeconds(300)
do {
 Start-Sleep -Seconds 2
 $temp=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String
 $temp.Trim()
 if($temp -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)' -and [double]$Matches[1] -ge 85){
  Stop-ScheduledTask BC250-M9-RadvMainBenchOld
  throw 'Temperature limit'
 }
 $state=(Get-ScheduledTask BC250-M9-RadvMainBenchOld).State
}while(-not (Test-Path "$out\worker.exit") -and (Get-Date) -lt $deadline)
'task_state='+$state
if(-not (Test-Path "$out\worker.exit")){throw 'Native worker unfinished, inspect before another run'}
'worker_exit='+(Get-Content "$out\worker.exit")
foreach($name in @('stories15M','tinyllama')) {
 $exit=[int](Get-Content "$out\$name.exit")
 "$name exit=$exit"
 if($exit -ne 0){throw "Failed $name"}
 $trace=Get-Content "$out\$name.err" -Raw
 if($trace -notmatch 'cache-intent-v2\\vulkan_radeon.dll' -or $trace -notmatch 'bc250: progress before submit'){
  throw "Missing loaded ICD witness: $name"
 }
 Get-Content "$out\$name.out" -Raw | ConvertFrom-Json | Select-Object n_prompt,n_gen,avg_ts,stddev_ts | Format-Table
}
& C:\BC250\m8\bc250kmd_cli.exe log summary | Out-File "$out\after.log"
Unregister-ScheduledTask BC250-M9-RadvMainBenchOld -Confirm:$false
'limited_run_complete'

'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')

& C:\BC250\m8\bc250kmd_cli.exe confirm
Get-Process dwm | Select-Object Id,StartTime,Responding | Format-Table
'final_time='+(Get-Date).ToString('s')
