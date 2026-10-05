$ErrorActionPreference='Stop'
. "$PSScriptRoot\transition-policy.ps1"
$c=@{success=$true;tree_closed=$true;reason='verified';health_scope='candidate-confirmed'}
$h=@{scope='candidate-confirmed';health=@{flags=15;ready_ms=60000;completed=1;age_ms=15000}}
if(!(Test-KmdDeploymentAcceptance $c $h)){throw 'Exact confirmed witness rejected'}
foreach($case in @(@('flags',7),@('ready_ms',59999),@('completed',0),@('age_ms',15001))){
 $bad=@{scope=$h.scope;health=$h.health.Clone()};$bad.health[$case[0]]=$case[1]
 if(Test-KmdDeploymentAcceptance $c $bad){throw 'Invalid health retained'}
}
foreach($case in @(@('success',$false),@('success','true'),@('tree_closed',$false),@('reason','no-budget'),@('health_scope','candidate-ready-only'))){
 $bad=$c.Clone();$bad[$case[0]]=$case[1]
 if(Test-KmdDeploymentAcceptance $bad $h){throw 'Invalid candidate retained'}
}
if(Test-KmdDeploymentAcceptance $c @{scope='restored-confirmed';health=$h.health}){throw 'Rollback witness used to retain candidate'}
if(Test-KmdDeploymentAcceptance $null $null){throw 'Missing receipt accepted'}
'PASS: confirmed retain admission and eleven false-retention controls'
