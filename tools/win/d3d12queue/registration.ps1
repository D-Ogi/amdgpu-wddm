# Reversible addition of the native DX12 slot. Caller owns the mutation mutex,
# persists this plan before Install, and bounds/verifies every writer process.
function Test-M15NamesEqual {
 param($Left,$Right)
 $a=@($Left);$b=@($Right)
 if($a.Count -ne $b.Count){return $false}
 for($i=0;$i -lt $a.Count;$i++) {if($a[$i] -isnot [string] -or $b[$i] -isnot [string] -or $a[$i] -cne $b[$i]){return $false}}
 return $true
}
function New-M15RegistrationPlan {
 param([Parameter(Mandatory)]$Before,[Parameter(Mandatory)][string]$Kind,[Parameter(Mandatory)][string]$Candidate)
 $names=@($Before)
 if($Kind -cne 'MultiString' -or $names.Count -ne 3){throw 'Expected exact three-slot REG_MULTI_SZ baseline'}
 foreach($name in $names){if($name -isnot [string] -or [string]::IsNullOrWhiteSpace($name)){throw 'Invalid baseline name'}}
 if($Candidate -notmatch '^[A-Za-z]:\\' -or [IO.Path]::GetFullPath($Candidate) -cne $Candidate -or
    [IO.Path]::GetFileName($Candidate) -cne 'amdgpu_wddm_d3d12.dll'){throw 'Expected canonical absolute candidate UMD path'}
 return [pscustomobject]@{schema=1;name='UserModeDriverName';kind='MultiString';before=[string[]]$names;
  candidate=$Candidate;after=[string[]]@($names[0],$names[1],$names[2],$Candidate)}
}
function Assert-M15RegistrationPlan {
 param($Plan)
 if(!$Plan -or $Plan.schema -ne 1 -or $Plan.name -cne 'UserModeDriverName' -or $Plan.kind -cne 'MultiString'){throw 'Invalid registration plan'}
 $expected=New-M15RegistrationPlan -Before $Plan.before -Kind $Plan.kind -Candidate $Plan.candidate
 if(!(Test-M15NamesEqual $Plan.after $expected.after)){throw 'Plan alters existing API slots'}
}
function Set-M15Registration {
 param([Parameter(Mandatory)]$Key,[Parameter(Mandatory)]$Plan,[Parameter(Mandatory)][ValidateSet('Install','Restore')][string]$Mode)
 Assert-M15RegistrationPlan $Plan
 if([string]$Key.GetValueKind($Plan.name) -cne $Plan.kind){throw 'Registry value kind changed'}
 $current=@($Key.GetValue($Plan.name))
 $baseline=Test-M15NamesEqual $current $Plan.before
 $installed=Test-M15NamesEqual $current $Plan.after
 if(!$baseline -and !$installed){throw 'Registration changed outside this transaction'}
 if($Mode -eq 'Install'){
  if(!$baseline){throw 'Candidate already installed; inspect, do not restart'}
  $desired=[string[]]$Plan.after
 }else{$desired=[string[]]$Plan.before}
 if(!(Test-M15NamesEqual $current $desired)){
  $Key.SetValue($Plan.name,$desired,[Microsoft.Win32.RegistryValueKind]::MultiString)
  $Key.Flush()
 }
 if([string]$Key.GetValueKind($Plan.name) -cne $Plan.kind -or
    !(Test-M15NamesEqual @($Key.GetValue($Plan.name)) $desired)){throw 'Registration readback mismatch'}
}
