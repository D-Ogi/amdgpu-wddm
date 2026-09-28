# Called by the supervisor; not an independent deployment entry point.
. "$PSScriptRoot\durable.ps1"
. "$PSScriptRoot\..\kmd168-transition\deadline.ps1"
. "$PSScriptRoot\..\kmd168-transition\invoke-bounded.ps1"
function Get-KmdChildClosure {
 param($Result)
 # A missing/malformed receipt is never a witness that descendants have stopped.
 try {$r=$Result.stdout | ConvertFrom-Json -ErrorAction Stop} catch {return 'unknown'}
 if($r -is [array] -or $r.job_empty -isnot [bool] -or !$r.job_empty -or
    $r.root_exit_observed -isnot [bool] -or $r.timed_out -isnot [bool] -or
    $r.termination_requested -isnot [bool] -or $r.child_pid -le 0){return 'unknown'}
 if($Result.exit_code -eq 0 -and $r.root_exit_observed -and !$r.timed_out -and $r.child_exit -eq 0){return 'success'}
 if($Result.exit_code -in @(124,126)){return 'failed-closed'}
 return 'unknown'
}
function Invoke-KmdTransitionArm {
 param([ValidateSet('candidate','restore')][string]$Arm,[string]$Directory,[string]$Tool,
       [long]$Origin,[long]$Frequency)
 $phases=if($Arm -eq 'candidate'){@('Capture','Disable','Install','Configure','Enable','Verify')}
         else{@('Disable','Install','Configure','Enable','Verify')}
 $phaseScript=Join-Path $PSScriptRoot 'phase.ps1'
 $powershell="$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe"
 foreach($phase in $phases){
  $elapsed=Get-KmdElapsed $Origin $Frequency
  $budget=Get-KmdChildBudgetMs $elapsed $Arm 30000
  if($budget -le 1000){return @{success=$false;tree_closed=$true;phase=$phase;reason='no-budget'}}
  $receipt=($Arm+'-'+$phase).ToLowerInvariant()
  $deadline=[Math]::Min($Origin+[long]($(if($Arm -eq 'candidate'){110}else{170})*$Frequency),
    [Diagnostics.Stopwatch]::GetTimestamp()+[long]($budget*$Frequency/1000))
  try {
   $result=Invoke-KmdBoundedChild -Tool $Tool -Deadline $deadline -Stdout "$Directory\$receipt.out" -Stderr "$Directory\$receipt.err" -Executable $powershell -Arguments @('-NoProfile','-File',$phaseScript,'-Phase',$phase,'-Arm',$Arm,'-Directory',$Directory,'-Receipt',$receipt,'-ChildDeadline',[string]$deadline)
  } catch {
   Write-DurableText "$Directory\$receipt-supervisor-error.txt" ([string]$_)
   return @{success=$false;tree_closed=$false;phase=$phase;reason='helper-exception'}
  }
  Write-DurableText "$Directory\$receipt-helper.json" ($result|ConvertTo-Json -Depth 6)
  $closure=Get-KmdChildClosure $result
  if($closure -ne 'success'){
   return @{success=$false;tree_closed=($closure -eq 'failed-closed');phase=$phase;reason=$closure}
  }
  # Child exit0 is insufficient without the phase's completion witness.
  try {
   $done=Get-Content "$Directory\$receipt-done.json" -Raw -ErrorAction Stop|ConvertFrom-Json -ErrorAction Stop
   if($done.phase -ne $phase -or $done.arm -ne $Arm -or [long]$done.qpc -lt $Origin -or [long]$done.qpc -gt $deadline){throw 'Invalid phase completion'}
  } catch {
   return @{success=$false;tree_closed=$true;phase=$phase;reason='completion-witness-invalid'}
  }
 }
 return @{success=$true;tree_closed=$true;phase='Verify';reason='verified'}
}
