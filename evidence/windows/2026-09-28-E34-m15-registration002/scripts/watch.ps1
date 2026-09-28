param([Parameter(Mandatory)][string]$ManifestSha256)
$ErrorActionPreference='Stop';$d=$PSScriptRoot
if($d -ine 'C:\BC250\m15\registration002'){throw 'Unexpected directory'}
$origin=[Diagnostics.Stopwatch]::GetTimestamp();$frequency=[Diagnostics.Stopwatch]::Frequency
. "$d\verify-stage.ps1"
. "$d\durable.ps1"
. "$d\invoke-bounded.ps1"
$null=Assert-KmdStage $d $ManifestSha256
$mutex=[Threading.Mutex]::new($false,'Global\BC250-M15-Registration');$locked=$false
$result=[ordered]@{status='failed';tree_closed=$true;phases=@();runtime_functional=$false;registered=$false;baseline_restored=$false}
function Phase($name,$budget,$end){
 $result.phases+=,$name
 $deadline=[Math]::Min($origin+$end*$frequency,[Diagnostics.Stopwatch]::GetTimestamp()+$budget*$frequency)
 $interactive=$name -eq 'Gpu'
 $r=Invoke-KmdBoundedChild -ActiveConsole:$interactive -Tool "$d\bounded-child.exe" -Deadline $deadline -Stdout "$d\$name.out" -Stderr "$d\$name.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\phase.ps1",'-Phase',$name)
 Write-DurableText "$d\$name-helper.json" ($r|ConvertTo-Json -Depth 5)
 $receipt=$r.stdout|ConvertFrom-Json
 $closed=$receipt -isnot [array] -and $receipt.job_empty -is [bool] -and $receipt.job_empty -and $receipt.child_pid -gt 0
 if(!$closed){$result.tree_closed=$false;return $false}
 return $r.exit_code -eq 0 -and $receipt.root_exit_observed -and !$receipt.timed_out -and $receipt.child_exit -eq 0 -and (!$interactive -or $receipt.console_session -gt 0) -and (Test-Path "$d\$name-done.json")
}
try{
 try{$locked=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{throw 'Abandoned mutation lock; inspect before retry'}
 if(!$locked){throw 'Registration mutation already active'}
 Write-DurableText "$d\start.json" (@{utc=[DateTime]::UtcNow.ToString('o');origin=$origin}|ConvertTo-Json)
 if(Phase 'Capture' 30 100){
  $ok=Phase 'Install' 35 100
  if($ok){$ok=Phase 'Cpu' 78 105}
  if($ok){$ok=Phase 'Gpu' 15 120}
  if($ok){$ok=Phase 'Verify' 25 145}
  if($ok){
   $result.status='registered-diagnostic';$result.registered=$true
   $result.runtime_functional=(Get-Content "$d\runtime-result.json" -Raw|ConvertFrom-Json).functional
  }elseif($result.tree_closed){
   if((Test-Path "$d\install-pnp-requested.json") -and !(Test-Path "$d\install-pnp-result.json")){
    $result.status='pnp-completion-unverified'
   }elseif(Phase 'Restore' 35 155){
    $result.baseline_restored=Phase 'VerifyRestored' 25 170
    $result.status=if($result.baseline_restored){'failed-restored'}else{'recovery-unverified'}
   }else{$result.status='recovery-unverified'}
  }
 }
 if(!$result.tree_closed){$result.status='closure-unverified'}
 $result.elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-$origin)/[double]$frequency
 Write-DurableText "$d\result.json" ($result|ConvertTo-Json -Depth 5)
 if($result.status -notin @('registered-diagnostic','failed-restored')){exit 1}
}finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
