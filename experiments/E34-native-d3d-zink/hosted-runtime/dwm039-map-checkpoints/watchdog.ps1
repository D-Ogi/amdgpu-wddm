$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted039'
. "$PSScriptRoot\durable.ps1"
Write-DurableText "$d\watchdog-ready.json" (@{pid=$PID;start=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json)
$preparation=[Diagnostics.Stopwatch]::StartNew()
$restoreError=$null
try {
 while(!(Test-Path "$d\trial-boundary.json")){
  if(Test-Path "$d\done.json"){return}
  if($preparation.Elapsed.TotalSeconds -gt 90){throw 'Preparation deadline'}
  Start-Sleep -Milliseconds 100
 }
 $boundary=Get-Content "$d\trial-boundary.json" -Raw|ConvertFrom-Json
 $start=[DateTime]::Parse($boundary.utc).ToUniversalTime()
 while(([DateTime]::UtcNow-$start).TotalSeconds -lt 140){
  if(Test-Path "$d\done.json"){return}
  Start-Sleep -Milliseconds 100
 }
 throw 'Independent rollback deadline'
} catch {
 Write-DurableText "$d\abort" ($_|Out-String)
 Get-ScheduledTask -TaskName BC250-G0-DwmRun039 -ErrorAction SilentlyContinue|Stop-ScheduledTask
 try {& "$d\restore.ps1" -Restart *> "$d\watchdog-restore.log"} catch {$restoreError=$_|Out-String}
 & logman stop BC250G0Dwm039 -ets *> "$d\etw-watchdog-stop.log"
 Get-ScheduledTask -TaskName BC250-G0-Composition039 -ErrorAction SilentlyContinue|Stop-ScheduledTask
} finally {
 Write-DurableText "$d\watchdog-done.json" (@{utc=[DateTime]::UtcNow.ToString('o');aborted=(Test-Path "$d\abort");restore_error=$restoreError}|ConvertTo-Json)
}

if($restoreError){throw $restoreError}
