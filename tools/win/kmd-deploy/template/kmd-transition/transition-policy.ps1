. "$PSScriptRoot\identity.ps1"
function Get-KmdTransitionPolicy {
 param([string]$Directory)
 $p=Get-Content "$Directory\transition-policy.json" -Raw -ErrorAction Stop|ConvertFrom-Json
 if($p.schema -ne 1 -or $p.mode -notin @($KmdCandidateMode,$KmdSameMode,$KmdDeployMode)){throw 'Invalid transition policy'}
 return $p.mode
}
function Get-KmdTransitionPhases {
 # ValidateSet needs literals; test-identity.ps1 proves they are identity.ps1's three modes.
 param([ValidateSet('candidate','restore')][string]$Arm,[ValidateSet('rehearsal','same','deploy')][string]$Mode)
 if($Mode -eq $KmdSameMode){
  if($Arm -eq 'candidate'){return @('Capture','Disable','Install')}
  return @('Quiesce','Rebind','Configure','Enable','Verify')
 }
 if($Arm -eq 'candidate'){return @('Capture','Disable','Install','Configure','Enable','Verify')}
 return @('Quiesce','Disable','Install','Configure','Enable','Verify','CleanupPackage')
}

function Test-KmdDeploymentAcceptance {
 param($Candidate,$Health)
 return ($Candidate.success -is [bool] -and $Candidate.success -and
  $Candidate.tree_closed -is [bool] -and $Candidate.tree_closed -and
  $Candidate.reason -eq 'verified' -and $Candidate.health_scope -eq 'candidate-confirmed' -and
  $Health.scope -eq 'candidate-confirmed' -and $Health.health.flags -eq 15 -and
  $Health.health.ready_ms -ge 60000 -and $Health.health.completed -gt 0 -and $Health.health.age_ms -le 15000)
}
