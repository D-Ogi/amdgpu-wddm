$ErrorActionPreference='Stop'
. "$PSScriptRoot\pnp-idle.ps1"
Assert-KmdPnpIdleResult @{wait_status=[uint32]0}
foreach($bad in @($null,@{},@{wait_status='0'},@{wait_status=[uint32]258},@{wait_status=[uint32]::MaxValue},@{wait_status=[uint32]1})){
 $rejected=$false
 try{Assert-KmdPnpIdleResult $bad}catch{$rejected=$true}
 if(!$rejected){throw 'Unknown/busy PnP state accepted'}
}
'PASS: explicit WAIT_OBJECT_0 only; pending/error/missing/untyped rejected'

foreach($problem in @(0,10,22,31,43)){Assert-KmdRestorableProblem $problem}
foreach($problem in @($null,1,28,52)){
 $rejected=$false;try{Assert-KmdRestorableProblem $problem}catch{$rejected=$true}
 if(!$rejected){throw 'Unknown device problem admitted'}
}
$frequency=[Diagnostics.Stopwatch]::Frequency
$budget=Get-KmdPnpWaitMs ([Diagnostics.Stopwatch]::GetTimestamp()+20*$frequency)
if($budget -ne 10000){throw 'PnP cap failed'}
$budget=Get-KmdPnpWaitMs ([Diagnostics.Stopwatch]::GetTimestamp()+6*$frequency)
if($budget -le 0 -or $budget -gt 1000){throw 'PnP deadline reserve failed'}
$rejected=$false;try{Get-KmdPnpWaitMs ([Diagnostics.Stopwatch]::GetTimestamp()+4*$frequency)|Out-Null}catch{$rejected=$true}
if(!$rejected){throw 'PnP wait admitted without reserve'}
'PASS: failed-start/add/post-start admitted, unknown problems rejected, finite PnP budget'
