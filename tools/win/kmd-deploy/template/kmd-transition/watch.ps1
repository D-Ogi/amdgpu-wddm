# Scheduled independently of SSH. Owns candidate execution and restoration.
param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$Tool,
 [Parameter(Mandatory)][long]$Origin,[Parameter(Mandatory)][long]$Frequency)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\identity.ps1"
. "$PSScriptRoot\supervise.ps1"
. "$PSScriptRoot\logged-transition.ps1"
$Directory=[IO.Path]::GetFullPath($Directory)
if($Directory -notmatch $KmdDirectoryPattern){throw 'Unexpected directory'}
if(Test-Path "$Directory\boundary.json"){throw 'Existing attempt; do not restart'}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
[void](Get-KmdElapsed $Origin $Frequency)
Write-DurableText "$Directory\boundary.json" (@{boot=$boot;machine=$env:COMPUTERNAME;qpc=$origin;frequency=$frequency}|ConvertTo-Json)
$script:setupLogTreeUnknown=$false
$runLogging={
 param($mode)
 if($script:setupLogTreeUnknown){return $false}
 $name='setup-log-'+$mode.ToLowerInvariant()
 $seconds=if($mode -eq 'Enable'){10}else{5}
 $deadline=[Math]::Min($origin+175*$frequency,[Diagnostics.Stopwatch]::GetTimestamp()+$seconds*$frequency)
 $script:setupLogTreeUnknown=$true
 $r=Invoke-KmdBoundedChild -Tool $Tool -Deadline $deadline -Stdout "$Directory\$name.out" -Stderr "$Directory\$name.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',"$PSScriptRoot\setup-log-phase.ps1",'-Mode',$mode,'-Directory',$Directory)
 Write-DurableText "$Directory\$name-helper.json" ($r|ConvertTo-Json -Depth 5)
 $closure=Get-KmdChildClosure $r
 $script:setupLogTreeUnknown=($closure -eq 'unknown')
 if($closure -ne 'success'){return $false}
 $done=Get-Content "$Directory\$name-done.json" -Raw -ErrorAction Stop|ConvertFrom-Json
 return ($done.mode -eq $mode -and [long]$done.qpc -ge $origin -and [long]$done.qpc -lt $deadline)
}
$retain=((Get-KmdTransitionPolicy $Directory) -eq $KmdDeployMode)
$candidateSeconds=if($retain){125}else{90}
$result=Invoke-KmdLoggedTransition -RunLogging $runLogging -Transition {
 Invoke-KmdSupervisedTransition -RetainConfirmedCandidate:$retain -CandidateSeconds $candidateSeconds -Directory $Directory -Tool $Tool -Origin $origin -Frequency $frequency -Worker "$PSScriptRoot\worker.ps1" -WorkerArguments @('-Directory',$Directory,'-Tool',$Tool) -Restore {
 Invoke-KmdTransitionArm restore $Directory $Tool $origin $frequency
}
}
Write-DurableText "$Directory\watch-result.json" ($result|ConvertTo-Json -Depth 6)
if($result.status -ne 'closed' -or !$result.candidate_verified){exit 1}
exit 0
