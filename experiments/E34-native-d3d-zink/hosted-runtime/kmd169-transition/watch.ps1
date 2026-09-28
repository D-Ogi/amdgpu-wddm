# Scheduled independently of SSH. Owns candidate execution and restoration.
param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$Tool)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\supervise.ps1"
$Directory=[IO.Path]::GetFullPath($Directory)
if($Directory -notmatch '^C:\\BC250\\m13\\kmd169-[a-z0-9-]+$'){throw 'Unexpected directory'}
if(Test-Path "$Directory\boundary.json"){throw 'Existing attempt; do not restart'}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$frequency=[Diagnostics.Stopwatch]::Frequency
$origin=[Diagnostics.Stopwatch]::GetTimestamp()
Write-DurableText "$Directory\boundary.json" (@{boot=$boot;machine=$env:COMPUTERNAME;qpc=$origin;frequency=$frequency}|ConvertTo-Json)
$result=Invoke-KmdSupervisedTransition -Directory $Directory -Tool $Tool -Origin $origin -Frequency $frequency -Worker "$PSScriptRoot\worker.ps1" -WorkerArguments @('-Directory',$Directory,'-Tool',$Tool) -Restore {
 Invoke-KmdTransitionArm restore $Directory $Tool $origin $frequency
}
Write-DurableText "$Directory\watch-result.json" ($result|ConvertTo-Json -Depth 6)
if($result.status -ne 'closed' -or !$result.candidate_verified){exit 1}
exit 0
