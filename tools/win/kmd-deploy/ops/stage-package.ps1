# Runs on unit A through stage.py push, after the attempt tree is in place. Read-only preflight of the
# rollback baseline, then the candidate package into the DriverStore WITHOUT /install; both packages proven registered.
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}(-(?!1-)[1-9][0-9]*)?-deploy[0-9]{3}$')][string]$Attempt,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ManifestSha256)
$ErrorActionPreference='Stop'
$d="C:\BC250\m15\$Attempt"
. "$d\kmd-transition\identity.ps1"
. "$d\kmd-transition\verify-stage.ps1"
. "$d\kmd-transition\package-cleanup.ps1"
. "$d\kmd168-transition\invoke-bounded.ps1"
if($d -notmatch $KmdDirectoryPattern){throw 'Unexpected directory'}
# bounded-child.exe creates its output files with CREATE_NEW: a fresh directory per run, never a stale receipt.
$run=Join-Path $PSScriptRoot ('stage-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
New-Item -ItemType Directory $run|Out-Null
Assert-KmdStage $d $ManifestSha256|Out-Null
$r=Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+30*[Diagnostics.Stopwatch]::Frequency) -Stdout "$run\preflight.out" -Stderr "$run\preflight.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile','-File',"$d\kmd-transition\preflight.ps1")
$r|ConvertTo-Json -Compress
if($r.exit_code -ne 0){throw 'Preflight failed'}
$b=Get-Content "$run\preflight.out" -Raw|ConvertFrom-Json
@{utc=$b.utc;version=$b.version;health=$b.health;temperature=$b.temperature;umd_entries=@($b.umd_registration).Count;graphics_values=@($b.graphics_registration.PSObject.Properties.Name);hang_detector=$b.hang_detector}|ConvertTo-Json -Compress -Depth 5
$m=Get-Content "$d\package-hashes.json" -Raw|ConvertFrom-Json
# The rollback is the active package, so its published INF must already be in the store; nothing is added for it.
Select-KmdRegisteredPackage @(Get-KmdPublishedPackages) $m.$KmdRollbackLabel.'bc250kmd.inf'|ConvertTo-Json -Compress
$r=Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+30*[Diagnostics.Stopwatch]::Frequency) -Stdout "$run\add-driver.out" -Stderr "$run\add-driver.err" -Executable "$env:windir\System32\pnputil.exe" -Arguments @('/add-driver',"$d\$KmdCandidateLabel\bc250kmd.inf")
$r|ConvertTo-Json -Compress
if($r.exit_code -ne 0){throw 'Package staging failed'}
Select-KmdRegisteredPackage @(Get-KmdPublishedPackages) $m.$KmdCandidateLabel.'bc250kmd.inf'|ConvertTo-Json -Compress
