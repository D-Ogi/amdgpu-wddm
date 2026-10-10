param([Parameter(Mandatory=$true)][string]$Library,[Parameter(Mandatory=$true)][string]$WorkDir,[switch]$ExpectFailures)
$ErrorActionPreference='Stop'
. $Library
New-Item -ItemType Directory -Force $WorkDir | Out-Null
Set-Content -LiteralPath (Join-Path $WorkDir 'stub.exe') 'never executed'
$realCopyWorker = if (Test-Path Function:Start-ArmCopyWorker) { ${function:Start-ArmCopyWorker} } else { $null }
$script:checks=0;$script:failures=0
function Check([bool]$Ok,[string]$Name){$script:checks++;if(-not $Ok){$script:failures++;Write-Host "FAIL $Name"}}
$script:helpers=@()
function New-SlowWorker {
 $si=New-Object Diagnostics.ProcessStartInfo
 $si.FileName=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
 $si.Arguments='-NoProfile -NonInteractive -Command "Start-Sleep -Seconds 15"'
 $si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true
 $p=[Diagnostics.Process]::Start($si);$script:helpers+= $p;return $p
}
function taskkill.exe {Start-Sleep -Milliseconds 1500}
function Start-ArmTreeKiller {param($TargetId) New-SlowWorker}
$fake=[pscustomobject]@{Id=424242;HasExited=$false}
$fake|Add-Member ScriptMethod Kill {}
$fake|Add-Member ScriptMethod WaitForExit {param($ms) return $true}
$timer=[Diagnostics.Stopwatch]::StartNew()
$r=Stop-ArmProcessTree $fake -TimeoutSec 0.05
$killElapsed=$timer.Elapsed.TotalSeconds
Check ($killElapsed -lt 0.8) 'stalled tree helper respects 50ms budget plus scheduling allowance'
Check (-not $r.Confirmed) 'expired kill never confirms later target exit'
# Exercise the real Invoke-LabArm output-copy branch in both libraries.
function Get-ArmCleanupLeftSec {param($Deadline,$ReserveSec) return 0.05}
function Copy-Item {param($LiteralPath,$Destination,[switch]$Force) Start-Sleep -Milliseconds 1500;[IO.File]::Copy($LiteralPath,$Destination,$true)}
function Start-ArmCopyWorker {param($Source,$Target) New-SlowWorker}
$done=[pscustomobject]@{Id=424242;HasExited=$true;ExitCode=0}
$done|Add-Member ScriptMethod WaitForExit {param($ms) return $true}
$script:ready=$done
$script:where=$WorkDir
$launch={
 $stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
 [IO.File]::WriteAllText((Join-Path $script:where "copy-$stamp.out.txt"),'sentinel payload')
 return $script:ready
}
$sample={param($budget) [pscustomobject]@{Ok=$true;Tctl=60;Line='';Reason=''}}
$timer.Restart()
$r=Invoke-LabArm -Name copy -Dir $WorkDir -Exe stub.exe -BoundSec 10 -CleanupReserveSec 4 -Sampler $sample -Launcher $launch -Redirect kept.txt -ReportPath (Join-Path $WorkDir 'report.json')
$copyElapsed=$timer.Elapsed.TotalSeconds
Check ($copyElapsed -lt 0.9) 'stalled copy is isolated inside cleanup budget'
Check (-not $r.redirect_sha256) 'expired copy cannot publish accepted artifact hash'
foreach($p in $script:helpers){try{if(-not $p.HasExited){$p.Kill()};$null=$p.WaitForExit(2000)}catch{}}
if (-not $ExpectFailures) {
 Check ($r.exit_status -ne 0 -and $r.redirect_status -eq 'failed') 'payload success does not mask requested evidence failure'
 Set-Item Function:Start-ArmCopyWorker $realCopyWorker
 $source=Join-Path $WorkDir "source ' spaced.txt"
 $target=Join-Path $WorkDir "target ' spaced.txt"
 [IO.File]::WriteAllText($source,'positive-copy-content')
 $copied=Invoke-ArmBoundedCopyHash -Source $source -Target $target -BudgetSec 5
 Check $copied.Ok 'real copy/hash worker succeeds'
 Check ($copied.Hash -eq (Get-FileHash -LiteralPath $source).Hash) 'real copied hash matches source'
 Check $copied.TerminationConfirmed 'real copy worker exit observed'
 $empty=Invoke-ArmBoundedCopyHash -Source $source -Target (Join-Path $WorkDir 'must-not-exist') -BudgetSec 0
 Check (-not $empty.Ok -and -not (Test-Path (Join-Path $WorkDir 'must-not-exist'))) 'zero budget starts no copy'
}
$result=[ordered]@{library_sha256=(Get-FileHash $Library).Hash;checks=$script:checks;failures=$script:failures;kill_elapsed=$killElapsed;copy_elapsed=$copyElapsed;negative_control=[bool]$ExpectFailures}
$result|ConvertTo-Json|Set-Content (Join-Path $WorkDir 'result.json')
$result|ConvertTo-Json
if($ExpectFailures){if($script:failures -ne 4){exit 1};exit 0}
exit $script:failures
