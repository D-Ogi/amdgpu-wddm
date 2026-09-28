param([Parameter(Mandatory)][string]$ManifestSha256)
$ErrorActionPreference='Stop'
$d='C:\BC250\m14\window003'
if($PSScriptRoot -ine $d){throw 'Unexpected directory'}
$origin=[Diagnostics.Stopwatch]::GetTimestamp();$frequency=[Diagnostics.Stopwatch]::Frequency
. "$d\verify-stage.ps1"
. "$d\durable.ps1"
. "$d\invoke-bounded.ps1"
. "$d\supervise.ps1"
$null=Assert-KmdStage $d $ManifestSha256
if(Test-Path "$d\start.json"){throw 'Existing attempt; do not restart'}
Write-DurableText "$d\start.json" (@{utc=[DateTime]::UtcNow.ToString('o');origin=$origin;frequency=$frequency;debugged=$true}|ConvertTo-Json)
$script:probeIndex=0
$result=Invoke-M14RuntimeTrial {
 param($phase)
 $budget=switch($phase){Capture{20} Install{10} Cpu{38} Gpu{38} Restore{10} Verify{20}}
 $end=if($phase -in @('Restore','Verify')){170}else{125}
 $deadline=[Math]::Min($origin+$end*$frequency,[Diagnostics.Stopwatch]::GetTimestamp()+$budget*$frequency)
 $interactive=$phase -in @('Cpu','Gpu')
 $scriptFile=if($interactive){"interactive-phase.ps1"}else{"phase.ps1"}
 $monitor=$null
 if($interactive){
  $monitor={
   $script:probeIndex++
   $prefix="$d\$phase-thermal-$script:probeIndex"
   $remaining=($deadline-[Diagnostics.Stopwatch]::GetTimestamp())/[double]$frequency
   if($remaining -lt 6){throw 'Insufficient thermal/control budget'}
   $probeDeadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long](4*$frequency)
   $probe=Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline $probeDeadline -Stdout "$prefix.out" -Stderr "$prefix.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\thermal-probe.ps1")
   Write-DurableText "$prefix-helper.json" ($probe|ConvertTo-Json -Depth 5)
   $receipt=$probe.stdout|ConvertFrom-Json
   if($probe.exit_code -ne 0 -or $receipt.job_empty -isnot [bool] -or !$receipt.job_empty -or !$receipt.root_exit_observed -or $receipt.child_exit -ne 0 -or $receipt.timed_out){throw 'Bounded thermal probe failed'}
   $sample=Get-Content "$prefix.out" -Raw|ConvertFrom-Json
   if($null -eq $sample.temperature -or $sample.temperature -ge 85){throw 'Invalid thermal sample'}
  }
  # No client exists yet. Failed initial telemetry is a closed admission refusal.
  try{& $monitor}catch{
   Write-DurableText "$d\$phase-admission-error.txt" ($_|Out-String)
   return @{closed=$true;success=$false}
  }
 }
 $r=Invoke-KmdBoundedChild -Monitor $monitor -CancelFile "$d\$phase.cancel" -ActiveConsole:$interactive -Tool "$d\bounded-child.exe" -Deadline $deadline -Stdout "$d\$phase.out" -Stderr "$d\$phase.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$d\$scriptFile",'-Phase',$phase)
 Write-DurableText "$d\$phase-helper.json" ($r|ConvertTo-Json -Depth 5)
 $receipt=$r.stdout|ConvertFrom-Json
 $closed=$receipt -isnot [array] -and $receipt.job_empty -is [bool] -and $receipt.job_empty -and $receipt.child_pid -gt 0
 $sessionOk=!$interactive -or $receipt.console_session -gt 0
 $success=!$r.monitor_error -and !$receipt.cancelled -and $sessionOk -and $closed -and $r.exit_code -eq 0 -and $receipt.root_exit_observed -is [bool] -and $receipt.root_exit_observed -and $receipt.timed_out -is [bool] -and !$receipt.timed_out -and $receipt.child_exit -eq 0 -and (Test-Path "$d\$phase-done.json")
 return @{closed=[bool]$closed;success=[bool]$success}
}
$result.elapsed=([Diagnostics.Stopwatch]::GetTimestamp()-$origin)/[double]$frequency
Write-DurableText "$d\result.json" ($result|ConvertTo-Json -Depth 5)
if($result.status -ne 'passed'){exit 1}
