param([Parameter(Mandatory)][string]$Tool,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\supervise.ps1"
if(Test-Path $Out){throw 'Use a fresh output directory'}
[void](New-Item -ItemType Directory $Out)
$results=@()
foreach($mode in @('ok','cancel','fail','tree','restore-fail')){
 $directory=Join-Path $Out $mode
 [void](New-Item -ItemType Directory $directory)
 $frequency=[Diagnostics.Stopwatch]::Frequency;$origin=[Diagnostics.Stopwatch]::GetTimestamp()
 $fixtureMode=if($mode -eq 'restore-fail'){'fail'}else{$mode}
 $result=Invoke-KmdSupervisedTransition -Directory $directory -Tool $Tool -Origin $origin -Frequency $frequency -CandidateSeconds 5 -Worker "$PSScriptRoot\supervisor-fixture.ps1" -WorkerArguments @('-Mode',$fixtureMode,'-Directory',$directory,'-Tool',$Tool) -Restore {
  if(!(Test-Path "$directory\restore-admitted.json")){throw 'No admission witness'}
  if($mode -eq 'tree'){
   $identity=Get-Content "$directory\descendant.json" -Raw|ConvertFrom-Json
   $live=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
   if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $identity.start){throw 'Concurrent rollback and candidate descendant'}
  }
  if($mode -eq 'restore-fail'){return @{success=$false;tree_closed=$true}}
  Write-DurableText "$directory\mock-restored.json" '{}'
  return @{success=$true;tree_closed=$true}
 }
 $expected=if($mode -eq 'cancel'){'cancelled'}elseif($mode -eq 'restore-fail'){'recovery-required'}else{'closed'}
 if($result.status -ne $expected){throw "Unexpected $mode outcome: $($result|ConvertTo-Json -Compress)"}
 if($mode -ne 'restore-fail' -and $result.candidate_verified -ne ($mode -eq 'ok')){throw "Candidate verification incorrect: $mode"}
 if($mode -eq 'cancel' -and (Test-Path "$directory\mock-restored.json")){throw 'Rollback before any mutation'}
 if((Get-KmdElapsed $origin $frequency) -gt 6){throw 'Exceeded fixture budget'}
 $results+=@{mode=$mode;result=$result}
}
Write-DurableText "$Out\results.json" ($results|ConvertTo-Json -Depth 8)
'PASS: supervisor success, cancellation, failed candidate and nested-tree timeout, rejected rollback failure'
