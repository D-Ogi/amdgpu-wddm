$ErrorActionPreference='Stop'
. "$PSScriptRoot\deadline.ps1"
$cases=@(
 @(0,$false,$false,$false,$false,'observe'),
 @(109.999,$false,$false,$false,$true,'observe'),
 @(110,$false,$false,$false,$true,'stop-worker'),
 @(111,$false,$true,$false,$true,'restore'),
 @(179.999,$false,$true,$false,$true,'restore'),
 @(180,$false,$false,$false,$true,'recovery-required'),
 @(180,$false,$true,$false,$true,'recovery-required'),
 @(20,$true,$false,$false,$false,'cancel'),
 @(20,$true,$false,$false,$true,'stop-worker'),
 @(20,$true,$true,$false,$true,'restore'),
 @(20,$false,$true,$false,$false,'cancel'),
 @(179,$false,$true,$true,$true,'closed')
)
foreach($c in $cases){$v=Get-KmdTransitionDecision $c[0] $c[1] $c[2] $c[3] $c[4];if($v -ne $c[5]){throw "Expected $($c[5]), got $v"}}
if((Get-KmdChildBudgetMs 109.5 candidate 3000) -ne 500){throw 'Candidate budget escaped deadline'}
if((Get-KmdChildBudgetMs 110 candidate 3000) -ne 0){throw 'Candidate admission after cutoff'}
if((Get-KmdChildBudgetMs 179.5 restore 3000) -ne 500){throw 'Recovery budget escaped deadline'}
if((Get-KmdChildBudgetMs 180 restore 3000) -ne 0){throw 'Child admission after180'}
foreach($bad in @(-1,[double]::NaN,[double]::PositiveInfinity)){
 $rejected=$false;try{Get-KmdTransitionDecision $bad $false $false $false $false|Out-Null}catch{$rejected=$true};if(!$rejected){throw 'Invalid clock accepted'}
}
'PASS:12 state cases,4 child-budget boundaries,3 invalid clocks'
