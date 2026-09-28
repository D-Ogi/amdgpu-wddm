$ErrorActionPreference='Stop'
. "$PSScriptRoot\confirmed-present-start.ps1"
$header=Get-Content (Join-Path $PSScriptRoot '..\..\..\driver\kmd\bc250kmd_escape.h') -Raw
foreach($name in 'Full','Ready','Visible','Confirmed') {
 $pattern='#define BC250_START_HEALTH_'+$name.ToUpperInvariant()+'\s+(\d+)u'
 if($header -notmatch $pattern){throw "Missing contract flag $name"}
 $expected=[int]$Matches[1]
 $actual=[int][Enum]::Parse([Bc250StartHealthFlags],$name)
 if($actual -ne $expected){throw "PowerShell flag differs from KMD contract: $name"}
}
$early='health abi=1 version=0x000700A4 flags=7 generation=109380886765 epoch=5 completed=65 age_ms=59 ready_ms=3947'
if((Get-ConfirmedPresentStart -Health $early -ElapsedSeconds 5).launch){throw 'Premature launch'}
$good=$early.Replace('flags=7','flags=15').Replace('ready_ms=3947','ready_ms=65000')
if(!(Get-ConfirmedPresentStart -Health $good -ElapsedSeconds 65 -ExpectedGeneration 109380886765 -ExpectedEpoch 5).launch){throw 'Confirmed control did not launch'}
$cases=@(@{h=$early;t=90;g=0;e=0},@{h=$good;t=65;g=99;e=5},@{h=$good;t=65;g=109380886765;e=6},@{h=$good.Replace('age_ms=59','age_ms=15001');t=65;g=0;e=0},@{h=$good.Replace('flags=15','flags=3');t=65;g=0;e=0},@{h=$early.Replace('flags=7','flags=15');t=5;g=0;e=0})
foreach($case in $cases){$threw=$false;try{Get-ConfirmedPresentStart -Health $case.h -ElapsedSeconds $case.t -ExpectedGeneration $case.g -ExpectedEpoch $case.e|Out-Null}catch{$threw=$true};if(!$threw){throw 'Invalid witness accepted'}}
'PASS observed flags7 waits, flags15 launches, deadline/generation/epoch/freshness/readiness failures reject'
