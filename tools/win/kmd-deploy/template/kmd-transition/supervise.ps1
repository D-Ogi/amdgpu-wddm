# The watchdog owns an outer job containing the worker and all nested phase jobs.
. "$PSScriptRoot\run-arm.ps1"
function Invoke-KmdSupervisedTransition {
 param([string]$Directory,[string]$Tool,[long]$Origin,[long]$Frequency,
       [string]$Worker,[string[]]$WorkerArguments,[scriptblock]$Restore,
       [ValidateRange(3,125)][int]$CandidateSeconds=90,[switch]$RetainConfirmedCandidate)
 $elapsed=Get-KmdElapsed $Origin $Frequency
 if($elapsed -ge $CandidateSeconds-1){throw 'No candidate launch budget'}
 $deadline=$Origin+[long]($CandidateSeconds*$Frequency)
 $powershell="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
 try {
  $result=Invoke-KmdBoundedChild -Tool $Tool -Deadline $deadline -Stdout "$Directory\worker.out" -Stderr "$Directory\worker.err" -Executable $powershell -Arguments (@('-NoProfile','-File',$Worker)+$WorkerArguments)
  Write-DurableText "$Directory\worker-helper.json" ($result|ConvertTo-Json -Depth 6)
  $closure=Get-KmdChildClosure $result
 }catch{
  Write-DurableText "$Directory\watch-error.txt" ([string]$_)
  $closure='unknown'
 }
 if($closure -eq 'unknown'){
  return @{status='recovery-required';reason='candidate-tree-unconfirmed';restored=$false}
 }
 # Even worker exit0 is not a verified candidate without its own result receipt.
 $candidateValid=$false
 if($closure -eq 'success'){
  try{
   $candidate=Get-Content "$Directory\candidate-result.json" -Raw -ErrorAction Stop|ConvertFrom-Json -ErrorAction Stop
   $candidateValid=($candidate.success -is [bool] -and $candidate.success -and
    $candidate.tree_closed -is [bool] -and $candidate.tree_closed -and $candidate.reason -eq 'verified')
  }catch{$candidateValid=$false}
 }
 if(!(Test-Path "$Directory\mutation-start.json")){
  return @{status='cancelled';reason='no-mutation';restored=$false;candidate_verified=$false}
 }
 if($RetainConfirmedCandidate -and $candidateValid){
  try{
   $acceptance=Get-Content "$Directory\candidate-verify-health-acceptance.json" -Raw -ErrorAction Stop|ConvertFrom-Json -ErrorAction Stop
   if(Test-KmdDeploymentAcceptance $candidate $acceptance){
    return @{status='closed';restored=$false;candidate_verified=$true;candidate_retained=$true;elapsed=(Get-KmdElapsed $Origin $Frequency)}
   }
  }catch{Write-DurableText "$Directory\retain-rejected.txt" ([string]$_)}
 }
 if((Get-KmdElapsed $Origin $Frequency) -ge 170){
  return @{status='recovery-required';reason='no-restore-budget';restored=$false}
 }
 # Process closure is necessary but not PnP admission. The restore arm first
 # checks pending OS installations and device state inside a bounded child.
 Write-DurableText "$Directory\candidate-tree-closed.json" (@{closure=$closure;qpc=[Diagnostics.Stopwatch]::GetTimestamp()}|ConvertTo-Json)
 try{$restored=& $Restore}catch{
  Write-DurableText "$Directory\restore-error.txt" ([string]$_)
  return @{status='recovery-required';reason='restore-exception';restored=$false}
 }
 $finished=Get-KmdElapsed $Origin $Frequency
 if($restored.success -isnot [bool] -or !$restored.success -or
    $restored.tree_closed -isnot [bool] -or !$restored.tree_closed -or $finished -gt 170){
  return @{status='recovery-required';reason='restore-unverified';restored=$false;elapsed=$finished}
 }
 return @{status='closed';restored=$true;candidate_verified=$candidateValid;elapsed=$finished}
}
