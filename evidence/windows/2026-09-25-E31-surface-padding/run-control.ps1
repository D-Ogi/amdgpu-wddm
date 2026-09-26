param(
 [Parameter(Mandatory=$true)][string]$InteractiveUser,
 [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedUmdSha256
)
$ErrorActionPreference='Stop'
$out='C:\BC250\m10\surface-padding'
$exe="$out\render_probe_padding.exe"
$log="$out\render-probe.txt"
$task='BC250-Cache147-Control-'+[Guid]::NewGuid().ToString('N')
if(Test-Path -LiteralPath $log){throw 'Preserve the previous output first; refusing overwrite'}
if(-not(Test-Path -LiteralPath $exe)){throw 'Probe executable missing'}
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
# Caller supplies a verified existing interactive identity. No credential or account guess.
$principal=New-ScheduledTaskPrincipal -UserId $InteractiveUser -LogonType Interactive -RunLevel Highest
$action=New-ScheduledTaskAction -Execute $exe -WorkingDirectory $out
$settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 30)
$registered=$false
try {
 Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
 $registered=$true
 $before=(Get-ScheduledTaskInfo $task).LastRunTime
 Start-ScheduledTask $task
 $deadline=[DateTime]::UtcNow.AddSeconds(35)
 do {
  Start-Sleep -Milliseconds 100
  $info=Get-ScheduledTaskInfo $task
  $state=(Get-ScheduledTask $task).State
  if($info.LastRunTime -gt $before -and $state -ne 'Running' -and $state -ne 'Queued'){break}
 }while([DateTime]::UtcNow -lt $deadline)
 if($info.LastRunTime -le $before -or $state -eq 'Running' -or $state -eq 'Queued'){throw 'Interactive launch/completion deadline'}
 'task_exit='+$info.LastTaskResult
 if(Test-Path -LiteralPath $log){Get-Content -LiteralPath $log}
 if($info.LastTaskResult -ne 0){throw 'Probe failed'}
 $content=Get-Content -LiteralPath $log -Raw
 foreach($required in @('completion A-clear PASS','completion B-clear PASS','completion green-copy PASS','shared A-to-B red mismatches 0 of 2048 extent 64x32','shared B-to-A blue mismatches 0 of 2048 extent 64x32','shared A-to-B red mismatches 0 of 47124 extent 1428x33','shared B-to-A blue mismatches 0 of 47124 extent 1428x33','shared A-to-B red mismatches 0 of 47810 extent 1366x35','shared B-to-A blue mismatches 0 of 47810 extent 1366x35','shared A-to-B red mismatches 0 of 1 extent 1x1','shared B-to-A blue mismatches 0 of 1 extent 1x1','shared A-to-B red mismatches 0 of 4355 extent 67x65','shared B-to-A blue mismatches 0 of 4355 extent 67x65','shared control PASS','green mismatches 0 of 307200','device removed 00000000')) {
  if(-not $content.Contains($required)){throw "Missing positive control: $required"}
 }
 $moduleLine=@(Get-Content -LiteralPath $log | Where-Object {$_ -like 'UMD_MODULE=*'})
 if($moduleLine.Count -ne 1){throw 'No unique loaded UMD witness'}
 $modulePath=$moduleLine[0].Substring('UMD_MODULE='.Length)
 $hash=(Get-FileHash -LiteralPath $modulePath -Algorithm SHA256).Hash
 "loaded_umd_path=$modulePath"
 "loaded_umd_sha256=$hash"
 if($hash -ne $ExpectedUmdSha256){throw 'Loaded UMD file hash differs from expected'}
 'CACHE147_SHARED_PIXEL_CONTROL_PASS'
} finally {
 if($registered){
  if((Get-ScheduledTask $task).State -eq 'Running'){Stop-ScheduledTask $task}
  Unregister-ScheduledTask $task -Confirm:$false
 }
}
