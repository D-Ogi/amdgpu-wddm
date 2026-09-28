param([Parameter(Mandatory)][string]$ManifestSha256)
$ErrorActionPreference='Stop'
$d='C:\BC250\m14\window002'
if($PSScriptRoot -ine $d){throw 'Unexpected directory'}
$origin=[Diagnostics.Stopwatch]::GetTimestamp();$frequency=[Diagnostics.Stopwatch]::Frequency
. "$d\verify-stage.ps1"
. "$d\durable.ps1"
. "$d\invoke-bounded.ps1"
. "$d\supervise.ps1"
$null=Assert-KmdStage $d $ManifestSha256
if(Test-Path "$d\start.json"){throw 'Existing attempt; do not restart'}
Write-DurableText "$d\start.json" (@{utc=[DateTime]::UtcNow.ToString('o');origin=$origin;frequency=$frequency;debugged=$true}|ConvertTo-Json)
$result=Invoke-M14RuntimeTrial {
 param($phase)
 $budget=switch($phase){Capture{20} Install{10} Cpu{38} Gpu{38} Restore{10} Verify{20}}
 $end=if($phase -in @('Restore','Verify')){170}else{125}
 $deadline=[Math]::Min($origin+$end*$frequency,[Diagnostics.Stopwatch]::GetTimestamp()+$budget*$frequency)
 $r=Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline $deadline -Stdout "$d\$phase.out" -Stderr "$d\$phase.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\phase.ps1",'-Phase',$phase)
 Write-DurableText "$d\$phase-helper.json" ($r|ConvertTo-Json -Depth 5)
 $receipt=$r.stdout|ConvertFrom-Json
 $closed=$receipt -isnot [array] -and $receipt.job_empty -is [bool] -and $receipt.job_empty -and $receipt.child_pid -gt 0
 $success=$closed -and $r.exit_code -eq 0 -and $receipt.root_exit_observed -is [bool] -and $receipt.root_exit_observed -and $receipt.timed_out -is [bool] -and !$receipt.timed_out -and $receipt.child_exit -eq 0 -and (Test-Path "$d\$phase-done.json")
 return @{closed=[bool]$closed;success=[bool]$success}
}
$result.elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-$origin)/[double]$frequency
Write-DurableText "$d\result.json" ($result|ConvertTo-Json -Depth 5)
if($result.status -ne 'passed'){exit 1}
