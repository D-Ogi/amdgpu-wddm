$ErrorActionPreference='Stop'
. "$PSScriptRoot\hosted-startup-witness.ps1"
function Run-Control([string]$Mode){
 $state=@{ms=0;records=@();mode=$Mode}
 $read={
  $id='pid5608/startA';$hashes=@('router','umd');$created=$false;$stop=$false
  if($state.mode -eq 'delayed' -and $state.ms -ge 6000){$hashes+= 'icd';$created=$true}
  if($state.mode -eq 'no-create'){$hashes+='icd'}
  if($state.mode -eq 'wrong-icd'){$hashes+='other-icd';$created=$true}
  if($state.mode -eq 'changed' -and $state.ms -ge 1000){$id='pid5609/startB'}
  if($state.mode -eq 'lost' -and $state.ms -ge 1000){$id=$null}
  if($state.mode -eq 'stop'){$stop=$true}
  if($state.mode -eq 'late-ready'){$state.ms=31000;$hashes+='icd';$created=$true}
  return @{identity=$id;hashes=$hashes;create_success=$created;stop=$stop}
 }.GetNewClosure()
 $record={param($r) $state.records+=,$r}.GetNewClosure()
 $clock={return $state.ms}.GetNewClosure()
 $delay={$state.ms+=500}.GetNewClosure()
 $ok=$false;$errorText=''
 try {$result=Wait-HostedDwmWitness @('router','umd','icd') $read $record $clock $delay;$ok=$true}
 catch {$errorText=$_.Exception.Message}
 if($Mode -eq 'delayed'){
  if(!$ok -or $result.elapsed_ms -ne 6000 -or $state.records.Count -ne 13){throw 'Delayed readiness was not observed'}
 }elseif($ok){throw "False acceptance: $Mode"}
 if($Mode -in 'no-create','wrong-icd' -and $state.ms -ne 30000){throw 'Deadline not enforced'}
 if($Mode -in 'changed','lost' -and $state.ms -ne 1000){throw 'Identity change was not immediate'}
 if($Mode -eq 'stop' -and $state.ms -ne 0){throw 'STOP was delayed'}
 @{case=$Mode;pass=$true;elapsed_ms=$state.ms;samples=$state.records.Count;error=$errorText}
}
@('delayed','no-create','wrong-icd','changed','lost','stop','late-ready') | ForEach-Object {Run-Control $_} | ConvertTo-Json -Depth 5
