param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\transition-policy.ps1"
if(Test-Path $Out){throw 'Fresh output required'}
New-Item -ItemType Directory $Out|Out-Null
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
Must-Reject {Get-KmdTransitionPolicy $Out}
foreach($mode in @('same170','candidate171','deploy171')){
 @{schema=1;mode=$mode}|ConvertTo-Json|Set-Content "$Out\transition-policy.json"
 if((Get-KmdTransitionPolicy $Out) -ne $mode){throw 'Policy lost'}
}
foreach($value in @(@{schema=2;mode='same170'},@{schema=1;mode='unknown'},@{schema=1})){
 $value|ConvertTo-Json|Set-Content "$Out\transition-policy.json"
 Must-Reject {Get-KmdTransitionPolicy $Out}
}
$candidate=@(Get-KmdTransitionPhases candidate same170)
$restore=@(Get-KmdTransitionPhases restore same170)
if($candidate -contains 'Enable' -or $candidate -contains 'Verify' -or $candidate[-1] -ne 'Install'){throw 'Same170 control starts a candidate'}
if($restore -contains 'Install' -or $restore -contains 'CleanupPackage' -or $restore[-1] -ne 'Verify'){throw 'Same170 recovery repeats rejected installer or removes active package'}
if([array]::IndexOf($restore,'Configure') -ge [array]::IndexOf($restore,'Enable')){throw 'Recovery enable precedes configuration'}
'PASS: missing/unknown policy rejection and same-package control/recovery phase boundaries'
