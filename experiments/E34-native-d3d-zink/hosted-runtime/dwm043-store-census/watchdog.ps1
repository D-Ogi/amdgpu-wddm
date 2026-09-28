$ErrorActionPreference='Stop'
$d='C:\BC250\m13\dwm-hosted043'
. "$PSScriptRoot\durable.ps1"
Write-DurableText "$d\watchdog-ready.json" (@{pid=$PID;start=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json)
$preparation=[Diagnostics.Stopwatch]::StartNew()
$restoreError=$null
try {
 . "$PSScriptRoot\watchdog-state.ps1"
 while($true){
  $hasBoundary=Test-Path "$d\trial-boundary.json"
  $elapsed=$preparation.Elapsed.TotalSeconds
  if($hasBoundary){
   $boundary=Get-Content "$d\trial-boundary.json" -Raw|ConvertFrom-Json
   if($boundary.qpc_frequency -ne [Diagnostics.Stopwatch]::Frequency -or $boundary.qpc -le 0){throw 'Invalid monotonic deadline origin'}
   $elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-[long]$boundary.qpc)/[double]$boundary.qpc_frequency
   if($elapsed -lt 0){throw 'Invalid monotonic elapsed time'}
  }
  $hasDone=Test-Path "$d\done.json"
  $restored=$false
  if($hasDone){
   $done=Get-Content "$d\done.json" -Raw|ConvertFrom-Json
   $restored=($done.restoration_succeeded -eq $true -and (Test-Path "$d\restored.json") -and !(Test-Path "$d\interop-pending"))
  }
  $decision=Get-DwmWatchDecision $hasBoundary $elapsed $hasDone $restored
  if($decision -eq 'closed'){return}
  if($decision -eq 'recover'){throw 'Independent recovery required'}
  Start-Sleep -Milliseconds 100
 }
} catch {
 Write-DurableText "$d\abort" ($_|Out-String)
 try {Get-ScheduledTask -TaskName BC250-G0-DwmRun043 -ErrorAction SilentlyContinue|Stop-ScheduledTask} catch {$_|Out-String|Set-Content "$d\watchdog-stop-error.log"}
 try {& "$d\restore.ps1" -Restart *> "$d\watchdog-restore.log"} catch {$restoreError=$_|Out-String}
 & logman stop BC250G0Dwm043 -ets *> "$d\etw-watchdog-stop.log"
 Get-ScheduledTask -TaskName BC250-G0-Composition043 -ErrorAction SilentlyContinue|Stop-ScheduledTask
} finally {
 Write-DurableText "$d\watchdog-done.json" (@{utc=[DateTime]::UtcNow.ToString('o');aborted=(Test-Path "$d\abort");restore_error=$restoreError}|ConvertTo-Json)
}

if($restoreError){throw $restoreError}
