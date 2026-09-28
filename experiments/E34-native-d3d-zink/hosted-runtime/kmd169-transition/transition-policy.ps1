function Get-KmdTransitionPolicy {
 param([string]$Directory)
 $p=Get-Content "$Directory\transition-policy.json" -Raw -ErrorAction Stop|ConvertFrom-Json
 if($p.schema -ne 1 -or $p.mode -notin @('candidate169','same166')){throw 'Invalid transition policy'}
 return $p.mode
}
function Get-KmdTransitionPhases {
 param([ValidateSet('candidate','restore')][string]$Arm,[ValidateSet('candidate169','same166')][string]$Mode)
 if($Mode -eq 'same166'){
  if($Arm -eq 'candidate'){return @('Capture','Disable','Install')}
  return @('Quiesce','Rebind','Configure','Enable','Verify')
 }
 if($Arm -eq 'candidate'){return @('Capture','Disable','Install','Configure','Enable','Verify')}
 return @('Quiesce','Disable','Install','Configure','Enable','Verify','CleanupPackage')
}
