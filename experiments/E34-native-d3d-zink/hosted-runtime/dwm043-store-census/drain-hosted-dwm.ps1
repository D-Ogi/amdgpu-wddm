# Baseline DLLs and disabled router admission must precede this drain.
function Stop-HostedDwmBeforeInterop {
 param([Parameter(Mandatory)][string]$Directory)
 $processes=@(Get-Process dwm -ErrorAction SilentlyContinue)
 $identities=@()
 foreach($process in $processes) {
  # Hold the original Process handle; never re-target a recycled PID.
  $handle=$process.Handle
  $identities+=@{pid=$process.Id;start=$process.StartTime.ToUniversalTime().ToString('o')}
 }
 $tag=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')
 $receipt=Join-Path $Directory "restore-dwm-drain-$tag.json"
 Write-DurableText $receipt (@{utc=[DateTime]::UtcNow.ToString('o');phase='before-stop';processes=$identities} | ConvertTo-Json -Depth 5)
 foreach($process in $processes) {
  if(!$process.HasExited) {Stop-Process -InputObject $process -Force -ErrorAction Stop}
 }
 foreach($process in $processes) {
  if(!$process.WaitForExit(5000)){throw "DWM drain timeout for original PID $($process.Id); adapter transition not started"}
 }
 Write-DurableText ($receipt.Replace('.json','-done.json')) (@{utc=[DateTime]::UtcNow.ToString('o');phase='all-original-processes-exited';processes=$identities} | ConvertTo-Json -Depth 5)
 return $identities
}
