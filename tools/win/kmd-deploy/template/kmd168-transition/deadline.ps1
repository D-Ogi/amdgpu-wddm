# One monotonic budget for install, observation and restoration. PS5.1 compatible.
function Get-KmdTransitionDecision {
 param([double]$Elapsed,[bool]$StopRequested,[bool]$WorkerTerminal,[bool]$Restored,[bool]$MutationStarted)
 if([double]::IsNaN($Elapsed) -or [double]::IsInfinity($Elapsed) -or $Elapsed -lt 0){throw 'Invalid elapsed time'}
 if($WorkerTerminal -and $Restored){return 'closed'}
 if($Elapsed -ge 180){return 'recovery-required'}
 if($StopRequested -or $Elapsed -ge 110 -or $WorkerTerminal){
  if(!$MutationStarted){return 'cancel'}
  if(!$WorkerTerminal){return 'stop-worker'}
  return 'restore'
 }
 return 'observe'
}
function Get-KmdChildBudgetMs {
 param([double]$Elapsed,[ValidateSet('candidate','restore')][string]$Phase,[int]$RequestedMs)
 if([double]::IsNaN($Elapsed) -or [double]::IsInfinity($Elapsed) -or $Elapsed -lt 0 -or $RequestedMs -le 0){throw 'Invalid child budget'}
 $end=if($Phase -eq 'candidate'){110.0}else{180.0}
 $remaining=[math]::Floor(($end-$Elapsed)*1000)
 if($remaining -le 0){return 0}
 return [int][math]::Min($RequestedMs,$remaining)
}
function Get-KmdElapsed {
 param([long]$Origin,[long]$Frequency)
 if($Origin -le 0 -or $Frequency -ne [Diagnostics.Stopwatch]::Frequency){throw 'Invalid QPC origin'}
 $elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-$Origin)/[double]$Frequency
 if($elapsed -lt 0){throw 'QPC origin is in the future'}
 return $elapsed
}
