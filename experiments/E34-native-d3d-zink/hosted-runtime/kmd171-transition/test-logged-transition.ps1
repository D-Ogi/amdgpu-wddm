$ErrorActionPreference='Stop'
. "$PSScriptRoot\logged-transition.ps1"
foreach($mode in @('success','setup-failure','transition-failure','restore-failure')){
 $script:events=New-Object Collections.Generic.List[string]
 $result=Invoke-KmdLoggedTransition -RunLogging {
  param($stage);$script:events.Add($stage)
  return !(($stage -eq 'Enable' -and $mode -eq 'setup-failure') -or ($stage -eq 'Restore' -and $mode -eq 'restore-failure'))
 } -Transition {
  $script:events.Add('Transition')
  if($mode -eq 'transition-failure'){throw 'Synthetic worker failure'}
  @{status='closed';restored=$true;candidate_verified=$true}
 }
 $expected=if($mode -eq 'setup-failure'){'Enable,Restore'}else{'Enable,Transition,Restore'}
 if(($script:events -join ',') -ne $expected){throw 'Missing independent restore or unsafe launch'}
 if($mode -eq 'success' -and ($result.status -ne 'closed' -or !$result.logging_restored)){throw 'Success rejected'}
 if($mode -ne 'success' -and $result.status -ne 'recovery-required'){throw 'False closure'}
 if($mode -eq 'restore-failure' -and (!$result.restored -or $result.logging_restored)){throw 'GPU/logging recovery evidence conflated'}
}
'PASS: supervisor restores logging after setup/worker failure; logging failure prevents closure'
