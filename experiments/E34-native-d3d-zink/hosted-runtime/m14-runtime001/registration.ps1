# Registry transaction primitives. Callers own the cross-process mutation mutex
# and persist the validated plan before Install. No automatic registry discovery.
function Test-M14RegistrationEqual {
 param($Left,$Right)
 $a=@($Left);$b=@($Right)
 if($a.Count -ne $b.Count){return $false}
 for($i=0;$i -lt $a.Count;$i++){if($a[$i] -isnot [string] -or $b[$i] -isnot [string] -or $a[$i] -cne $b[$i]){return $false}}
 return $true
}
function New-M14RegistrationPlan {
 param([Parameter(Mandatory)]$Before,[Parameter(Mandatory)][string]$Kind)
 if($Kind -ne 'MultiString'){throw 'Expected REG_MULTI_SZ'}
 $beforeValues=@($Before)
 $cpu='C:\BC250\m11\resource-close\bc250d3d.dll'
 # This exact three-slot layout is the measured171 baseline. A new layout must
 # be reviewed rather than redirecting unknown runtime slots by filename alone.
 if(!(Test-M14RegistrationEqual $beforeValues @('bc250umd.dll',$cpu,$cpu))){throw 'Unknown runtime registration layout'}
 return [pscustomobject]@{schema=1;name='UserModeDriverName';kind='MultiString';before=[string[]]$beforeValues;
  after=[string[]]@($beforeValues[0],'C:\BC250\m14\runtime001\bc250d3d-router.dll','C:\BC250\m14\runtime001\bc250d3d-router.dll')}
}
function Assert-M14RegistrationPlan {
 param($Plan)
 if(!$Plan -or $Plan.schema -ne 1 -or $Plan.name -cne 'UserModeDriverName' -or $Plan.kind -cne 'MultiString'){throw 'Invalid registration plan'}
 $expected=New-M14RegistrationPlan -Before $Plan.before -Kind $Plan.kind
 if(!(Test-M14RegistrationEqual $Plan.after $expected.after)){throw 'Unexpected registration target'}
}
function Set-M14Registration {
 param([Parameter(Mandatory)]$Key,[Parameter(Mandatory)]$Plan,[ValidateSet('Install','Restore')][string]$Mode)
 Assert-M14RegistrationPlan $Plan
 if([string]$Key.GetValueKind($Plan.name) -ne $Plan.kind){throw 'Registration type changed'}
 $current=@($Key.GetValue($Plan.name))
 $baseline=Test-M14RegistrationEqual $current $Plan.before
 $routed=Test-M14RegistrationEqual $current $Plan.after
 if(!$baseline -and !$routed){throw 'Registration changed outside this transaction'}
 if($Mode -eq 'Install'){
  if(!$baseline){throw 'Router already installed; do not restart'}
  $desired=[string[]]$Plan.after
 }else{$desired=[string[]]$Plan.before}
 if(!(Test-M14RegistrationEqual $current $desired)){
  $Key.SetValue($Plan.name,$desired,[Microsoft.Win32.RegistryValueKind]::MultiString)
  $Key.Flush()
 }
 if([string]$Key.GetValueKind($Plan.name) -ne $Plan.kind -or
    !(Test-M14RegistrationEqual @($Key.GetValue($Plan.name)) $desired)){throw 'Registration readback mismatch'}
}
