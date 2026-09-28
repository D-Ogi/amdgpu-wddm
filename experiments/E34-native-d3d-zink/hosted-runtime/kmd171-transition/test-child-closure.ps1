$ErrorActionPreference='Stop'
. "$PSScriptRoot\run-arm.ps1"
function New-Result {
 param([int]$Code=0)
 $r=@{child_pid=42;root_exit_observed=$true;child_exit=0;timed_out=$false;termination_requested=$true;job_empty=$true}
 return @{exit_code=$Code;stdout=($r|ConvertTo-Json -Compress);stderr=''}
}
if((Get-KmdChildClosure (New-Result)) -ne 'success'){throw 'Positive closure rejected'}
foreach($code in @(124,126)){
 $result=New-Result $code
 if((Get-KmdChildClosure $result) -ne 'failed-closed'){throw 'Closed failure not recognized'}
}
$bad=@(
 @{exit_code=0;stdout=''},
 @{exit_code=0;stdout='not-json'},
 @{exit_code=0;stdout='{"job_empty":true}'},
 @{exit_code=0;stdout='{"job_empty":"true"}'},
 (New-Result 125)
)
foreach($field in @('job_empty','root_exit_observed')) {
 $result=New-Result;$body=$result.stdout|ConvertFrom-Json;$body.$field=$false
 $result.stdout=$body|ConvertTo-Json -Compress;$bad+=$result
}
$result=New-Result;$body=$result.stdout|ConvertFrom-Json;$body.timed_out=$true
$result.stdout=$body|ConvertTo-Json -Compress;$bad+=$result
foreach($result in $bad){if((Get-KmdChildClosure $result) -ne 'unknown'){throw 'False tree closure'}}
'PASS: successful closure, two closed failures, eight uncertain receipts'
