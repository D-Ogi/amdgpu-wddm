$ErrorActionPreference='Stop'
. "$PSScriptRoot\registration.ps1"
function Reject([scriptblock]$Action){$failed=$false;try{& $Action}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
$cpu='C:\BC250\m11\resource-close\bc250d3d.dll'
$before=[string[]]@('bc250umd.dll',$cpu,$cpu)
$plan=New-M14RegistrationPlan $before 'MultiString'
# Actual JSON persistence round trip, preserving arrays and type.
$plan=$plan|ConvertTo-Json -Depth 4|ConvertFrom-Json
Assert-M14RegistrationPlan $plan
foreach($bad in @(@($cpu,$cpu),@('other.dll',$cpu,$cpu),@('bc250umd.dll',$cpu,'other.dll'),@('bc250umd.dll',$cpu,$cpu,$cpu))){Reject {New-M14RegistrationPlan $bad 'MultiString'}}
Reject {New-M14RegistrationPlan $before 'String'}
$key=[pscustomobject]@{values=[string[]]$before;kind='MultiString';writes=0;flushed=0;drop=$false}
$key|Add-Member ScriptMethod GetValueKind {param($n) if($n -ne 'UserModeDriverName'){throw 'Wrong key'};return $this.kind}
$key|Add-Member ScriptMethod GetValue {param($n) if($n -ne 'UserModeDriverName'){throw 'Wrong key'};return ,$this.values}
$key|Add-Member ScriptMethod SetValue {param($n,$v,$k) if($n -ne 'UserModeDriverName' -or [string]$k -ne 'MultiString'){throw 'Wrong write'};$this.writes++;if(!$this.drop){$this.values=[string[]]$v}}
$key|Add-Member ScriptMethod Flush {$this.flushed++}
Set-M14Registration $key $plan Install
if($key.writes -ne 1 -or $key.flushed -ne 1 -or $key.values[0] -cne 'bc250umd.dll'){throw 'Install or first slot changed'}
Reject {Set-M14Registration $key $plan Install}
Set-M14Registration $key $plan Restore
Set-M14Registration $key $plan Restore
if($key.writes -ne 2 -or !(Test-M14RegistrationEqual $key.values $before)){throw 'Restore not exact/idempotent'}
$key.kind='String';Reject {Set-M14Registration $key $plan Install};$key.kind='MultiString'
$key.values=[string[]]@('unknown');Reject {Set-M14Registration $key $plan Restore}
if($key.writes -ne 2){throw 'Clobbered foreign state'}
$key.values=[string[]]$before;$key.drop=$true;Reject {Set-M14Registration $key $plan Install}
$plan.after[0]='bad.dll';Reject {Assert-M14RegistrationPlan $plan}
'PASS exact slot layout, JSON roundtrip, type retention, install/restore, idempotence, foreign-state refusal and failed readback'
