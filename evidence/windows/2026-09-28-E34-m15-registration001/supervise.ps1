# RunPhase owns a bounded child Job Object and returns typed success/closed.
# Every file/registry-writing phase must be confirmed closed before another begins.
function Invoke-M14RuntimeTrial {
 param([Parameter(Mandatory)][scriptblock]$RunPhase)
 $trace=New-Object 'System.Collections.Generic.List[string]'
 $result=[ordered]@{status='failed';cpu_verified=$false;gpu_verified=$false;baseline_restored=$false;
  postflight_verified=$false;tree_closed=$true;failed_phase=$null;phases=@()}
 $scriptPhase={
  param($Name)
  $trace.Add($Name)
  try{$receipt=& $RunPhase $Name}catch{$receipt=$null}
  if(!$receipt -or $receipt.closed -isnot [bool] -or !$receipt.closed -or $receipt.success -isnot [bool]){
   $result.tree_closed=$false;$result.failed_phase=$Name;return $false
  }
  if(!$receipt.success){$result.failed_phase=$Name;return $false}
  return $true
 }
 if(!(& $scriptPhase 'Capture')){$result.phases=$trace.ToArray();return $result}
 # Even a failed Install can have modified the active route. Restore whenever
 # its writer is known to have stopped, regardless of the return code.
 $installed=& $scriptPhase 'Install'
 if($installed){
  $result.cpu_verified=& $scriptPhase 'Cpu'
  if($result.cpu_verified){$result.gpu_verified=& $scriptPhase 'Gpu'}
 }
 if($result.tree_closed){
  $result.baseline_restored=& $scriptPhase 'Restore'
  if($result.baseline_restored){$result.postflight_verified=& $scriptPhase 'Verify'}
 }
 if(!$result.tree_closed){$result.status='closure-unverified'}
 elseif(!$result.baseline_restored -or !$result.postflight_verified){$result.status='recovery-unverified'}
 elseif($result.cpu_verified -and $result.gpu_verified){$result.status='passed'}
 else{$result.status='failed-restored'}
 $result.phases=$trace.ToArray()
 return $result
}
