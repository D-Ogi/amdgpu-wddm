param([Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\invoke-bounded.ps1"
if(Test-Path $Out){throw 'Fresh test directory required'}
$null=New-Item -ItemType Directory $Out
$pidFile="$Out\child.json"
$deadline=[Diagnostics.Stopwatch]::GetTimestamp()+[long](8*[Diagnostics.Stopwatch]::Frequency)
$r=Invoke-KmdBoundedChild -Tool $Exe -Deadline $deadline -Stdout "$Out\child.out" -Stderr "$Out\child.err" -CancelFile "$Out\cancel" -Monitor {
 if(Test-Path $pidFile){throw 'Injected monitor failure'}
} -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',"$PSScriptRoot\bounded-child-fixture.ps1",'-Mode','tree','-PidFile',$pidFile)
$receipt=$r.stdout|ConvertFrom-Json
if($r.exit_code -ne 123 -or !$receipt.cancelled -or !$receipt.job_empty -or $r.monitor_error -ne 'Injected monitor failure'){throw "Bad monitored closure: $($r|ConvertTo-Json)"}
$saved=Get-Content $pidFile -Raw|ConvertFrom-Json
$live=Get-Process -Id $saved.pid -ErrorAction SilentlyContinue
if($live -and !$live.HasExited -and $live.StartTime.ToUniversalTime().ToString('o') -eq $saved.start){throw 'Cancelled descendant survived'}
$r|ConvertTo-Json|Set-Content "$Out\result.json"
'PASS monitor failure requests cancellation and receives confirmed empty Job'
