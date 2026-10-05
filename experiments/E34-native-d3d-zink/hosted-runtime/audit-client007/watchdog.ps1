. "$PSScriptRoot\common.ps1"
$code=125
try {
 Write-DurableText "$d\watchdog-ready.json" (@{pid=$PID;start=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')}|ConvertTo-Json)
 $watch=[Diagnostics.Stopwatch]::StartNew()
 while($watch.Elapsed.TotalSeconds -lt 90 -and !(Test-Path "$d\done.json") -and !(Test-Path "$d\restore-request")){Start-Sleep -Milliseconds 250}
 if(!(Test-Path "$d\done.json")){
  if(!(Test-Path "$d\abort")){Write-DurableText "$d\abort" 'watchdog restoration'}
  Get-ScheduledTask -TaskName $workerTask -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
 }
 & "$d\restore.ps1"
 $code=0
}catch{$_|Out-String|Add-Content "$d\watchdog.log"}
finally{Write-DurableText "$d\watchdog-done.json" (@{exit=$code;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json)}
exit $code
