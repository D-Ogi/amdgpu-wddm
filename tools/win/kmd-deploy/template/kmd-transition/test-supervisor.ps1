param([Parameter(Mandatory)][string]$Tool,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\supervise.ps1"
if(Test-Path $Out){throw 'Use a fresh output directory'}
[void](New-Item -ItemType Directory $Out)
# The nested-tree mode holds two timing races. Both are in the fixture's assumptions, not in the supervisor.
#  - Its budget must outlast four process starts (the worker powershell, bounded-child.exe, the nested powershell
#    and the descendant) before the watchdog empties the job. Under load a 5 s budget lost the descendant.json
#    write of bounded-child-fixture.ps1, and this test then threw inside its own Restore scriptblock
#    ('Cannot find path ... descendant.json', status restore-exception): 10 of 48 runs in 16 concurrent lanes on
#    one development PC, 0 of 36 in 6 lanes or one at a time. The tree mode therefore gets a budget of its own.
#  - bounded-child.exe's own deadline and the WaitForExit of the same deadline in invoke-bounded.ps1 are one
#    instant, so whichever gets there first decides the result. Either the helper reports its emptied job (exit
#    124, closure failed-closed, status closed), or the watchdog kills it and refuses to go on (closure unknown,
#    status recovery-required and reason candidate-tree-unconfirmed, with the kill recorded in watch-error.txt).
#    Both are correct supervisor behaviour, and on the lab the refusal is the safe answer, so the mode admits
#    both and proves the closure of the tree itself in each.
$budget=@{ok=5;cancel=5;fail=5;tree=12;'restore-fail'=5}
$results=@()
foreach($mode in @('ok','cancel','fail','tree','restore-fail')){
 $directory=Join-Path $Out $mode
 [void](New-Item -ItemType Directory $directory)
 $frequency=[Diagnostics.Stopwatch]::Frequency;$origin=[Diagnostics.Stopwatch]::GetTimestamp()
 $fixtureMode=if($mode -eq 'restore-fail'){'fail'}else{$mode}
 $seconds=[int]$budget[$mode]
 # The descendant bounded-child-fixture.ps1 recorded must be gone: same pid and same start time means it lives.
 $treeClosed={
  param($where)
  $path="$where\descendant.json"
  if(!(Test-Path $path)){throw "The tree fixture did not reach its descendant inside $seconds s; raise the tree budget ($path)"}
  $identity=Get-Content $path -Raw|ConvertFrom-Json
  $live=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
  if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Candidate descendant still running'}
 }
 $result=Invoke-KmdSupervisedTransition -Directory $directory -Tool $Tool -Origin $origin -Frequency $frequency -CandidateSeconds $seconds -Worker "$PSScriptRoot\supervisor-fixture.ps1" -WorkerArguments @('-Mode',$fixtureMode,'-Directory',$directory,'-Tool',$Tool) -Restore {
  if(!(Test-Path "$directory\candidate-tree-closed.json")){throw 'No admission witness'}
  if($mode -eq 'tree'){& $treeClosed $directory}
  if($mode -eq 'restore-fail'){return @{success=$false;tree_closed=$true}}
  Write-DurableText "$directory\mock-restored.json" '{}'
  return @{success=$true;tree_closed=$true}
 }
 $expected=if($mode -eq 'cancel'){'cancelled'}elseif($mode -eq 'restore-fail'){'recovery-required'}else{'closed'}
 $killed=($mode -eq 'tree' -and $result.status -eq 'recovery-required' -and $result.reason -eq 'candidate-tree-unconfirmed')
 if($killed){
  # The watchdog won the deadline race: no receipt of an emptied job exists, so it restored nothing. The tree
  # must be closed all the same, and the kill must be on record.
  & $treeClosed $directory
  if(Test-Path "$directory\mock-restored.json"){throw 'Rollback after an unconfirmed tree'}
  if(!(Test-Path "$directory\watch-error.txt")){throw 'Unconfirmed tree without a recorded watch error'}
 }elseif($result.status -ne $expected){throw "Unexpected $mode outcome: $($result|ConvertTo-Json -Compress)"}
 if(!$killed -and $mode -ne 'restore-fail' -and $result.candidate_verified -ne ($mode -eq 'ok')){throw "Candidate verification incorrect: $mode"}
 if($mode -eq 'cancel' -and (Test-Path "$directory\mock-restored.json")){throw 'Rollback before any mutation'}
 if((Get-KmdElapsed $origin $frequency) -gt ($seconds+1)){throw 'Exceeded fixture budget'}
 $results+=@{mode=$mode;result=$result;watchdog_kill=$killed;budget_seconds=$seconds}
}
Write-DurableText "$Out\results.json" ($results|ConvertTo-Json -Depth 8)
'PASS: supervisor success, cancellation, failed candidate and nested-tree timeout, rejected rollback failure'
