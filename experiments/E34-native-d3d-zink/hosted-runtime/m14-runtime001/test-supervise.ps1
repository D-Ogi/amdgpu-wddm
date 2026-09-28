$ErrorActionPreference='Stop'
. "$PSScriptRoot\supervise.ps1"
function Check($Condition,[string]$Why){if(!$Condition){throw $Why}}
$r=Invoke-M14RuntimeTrial {param($p) @{success=$true;closed=$true}}
Check ($r.status -eq 'passed' -and ($r.phases -join ',') -eq 'Capture,Install,Cpu,Gpu,Restore,Verify') 'Happy path'
foreach($bad in @('Capture','Install','Cpu','Gpu','Restore','Verify')){
 $run={param($p) @{success=($p -ne $bad);closed=$true}}.GetNewClosure()
 $r=Invoke-M14RuntimeTrial $run
 if($bad -eq 'Capture'){Check (($r.phases -join ',') -eq 'Capture') 'Mutation after failed capture'}
 elseif($bad -in @('Install','Cpu','Gpu')){
  Check ($r.status -eq 'failed-restored' -and $r.registration_restored -and $r.postflight_verified) 'Did not recover failed test'
  if($bad -ne 'Gpu'){Check ('Gpu' -notin $r.phases) 'GPU admitted without CPU control'}
 }else{Check ($r.status -eq 'recovery-unverified') 'Recovery failure hidden'}
}
foreach($bad in @('Install','Cpu','Gpu')){
 foreach($form in @('false','string','throw','missing')){
  $run={param($p)
   if($p -ne $bad){return @{success=$true;closed=$true}}
   switch($form){'false'{return @{success=$false;closed=$false}} 'string'{return @{success=$true;closed='true'}} 'throw'{throw 'lost helper'} 'missing'{return $null}}
  }.GetNewClosure()
  $r=Invoke-M14RuntimeTrial $run
  Check ($r.status -eq 'closure-unverified' -and !$r.tree_closed -and 'Restore' -notin $r.phases) 'Concurrent restore after unknown closure'
 }
}
'PASS phased M14 supervisor: CPU admission, failed install/test recovery, restoration failure and unverified tree closure'
