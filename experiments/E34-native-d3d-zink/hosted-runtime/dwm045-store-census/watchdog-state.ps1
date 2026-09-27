# Pure state decision, shared with host fault controls.
function Get-DwmWatchDecision {
 param([bool]$HasBoundary,[double]$ElapsedSeconds,[bool]$HasDone,[bool]$Restored)
 if($HasDone){
  if(!$HasBoundary -or $Restored){return 'closed'}
  return 'recover'
 }
 if(($HasBoundary -and $ElapsedSeconds -ge 140) -or (!$HasBoundary -and $ElapsedSeconds -ge 90)){return 'recover'}
 return 'wait'
}
