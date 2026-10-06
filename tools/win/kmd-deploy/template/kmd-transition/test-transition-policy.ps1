param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\transition-policy.ps1"
if(Test-Path $Out){throw 'Fresh output required'}
New-Item -ItemType Directory $Out|Out-Null
function Must-Reject([scriptblock]$Action){$failed=$false;try{& $Action|Out-Null}catch{$failed=$true};if(!$failed){throw 'False acceptance'}}
Must-Reject {Get-KmdTransitionPolicy $Out}
foreach($mode in @($KmdSameMode,$KmdCandidateMode,$KmdDeployMode)){
 @{schema=1;mode=$mode}|ConvertTo-Json|Set-Content "$Out\transition-policy.json"
 if((Get-KmdTransitionPolicy $Out) -ne $mode){throw 'Policy lost'}
}
# The previous promotion's modes are not this promotion's.
foreach($value in @(@{schema=2;mode=$KmdSameMode},@{schema=1;mode='unknown'},@{schema=1},@{schema=1;mode='deploy174'},@{schema=1;mode='candidate'})){
 $value|ConvertTo-Json|Set-Content "$Out\transition-policy.json"
 Must-Reject {Get-KmdTransitionPolicy $Out}
}
$candidate=@(Get-KmdTransitionPhases candidate $KmdSameMode)
$restore=@(Get-KmdTransitionPhases restore $KmdSameMode)
if($candidate -contains 'Enable' -or $candidate -contains 'Verify' -or $candidate[-1] -ne 'Install'){throw 'Same-package control starts a candidate'}
if($restore -contains 'Install' -or $restore -contains 'CleanupPackage' -or $restore[-1] -ne 'Verify'){throw 'Same-package recovery repeats rejected installer or removes active package'}
if([array]::IndexOf($restore,'Configure') -ge [array]::IndexOf($restore,'Enable')){throw 'Recovery enable precedes configuration'}
foreach($mode in @($KmdCandidateMode,$KmdDeployMode)){
 $candidate=@(Get-KmdTransitionPhases candidate $mode);$restore=@(Get-KmdTransitionPhases restore $mode)
 if(($candidate -join ',') -ne 'Capture,Disable,Install,Configure,Enable,Verify'){throw "Candidate order changed: $mode"}
 if(($restore -join ',') -ne 'Quiesce,Disable,Install,Configure,Enable,Verify,CleanupPackage'){throw "Restore order changed: $mode"}
}
'PASS: missing/unknown/previous policy rejection, candidate and restore phase order, same-package control/recovery boundaries'
