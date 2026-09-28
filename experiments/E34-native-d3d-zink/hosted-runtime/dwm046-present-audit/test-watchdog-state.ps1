$ErrorActionPreference='Stop'
. "$PSScriptRoot\watchdog-state.ps1"
$cases=@(
 @{boundary=$false;elapsed=1;done=$false;restored=$false;expected='wait'},
 @{boundary=$false;elapsed=90;done=$false;restored=$false;expected='recover'},
 @{boundary=$false;elapsed=1;done=$true;restored=$false;expected='closed'},
 @{boundary=$true;elapsed=139.9;done=$false;restored=$false;expected='wait'},
 @{boundary=$true;elapsed=140;done=$false;restored=$false;expected='recover'},
 @{boundary=$true;elapsed=10;done=$true;restored=$false;expected='recover'},
 @{boundary=$true;elapsed=100;done=$true;restored=$true;expected='closed'}
)
foreach($case in $cases){
 $actual=Get-DwmWatchDecision $case.boundary $case.elapsed $case.done $case.restored
 if($actual -ne $case.expected){throw ('Wrong watchdog decision: '+($case|ConvertTo-Json -Compress))}
}
@{cases=$cases.Count;pass=$true;lab_used=$false}|ConvertTo-Json
