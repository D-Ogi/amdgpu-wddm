# Read-only diagnostic: runs ops/task-probe.ps1 (postflight.ps1 line 52, sampled) inside the same bounded child
# postflight uses (the attempt's bounded-child.exe and invoke-bounded.ps1, 30 s deadline), then once directly.
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}(-(?!1-)[1-9][0-9]*)?-deploy[0-9]{3}$')][string]$Attempt)
$ErrorActionPreference = 'Stop'
$d = "C:\BC250\m15\$Attempt"
. "$d\kmd168-transition\invoke-bounded.ps1"
$probe = "$PSScriptRoot\task-probe.ps1"
$run = Join-Path $PSScriptRoot ('task-probe-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
New-Item -ItemType Directory $run | Out-Null
$r = Invoke-KmdBoundedChild -Tool "$d\bounded-child.exe" -Deadline ([Diagnostics.Stopwatch]::GetTimestamp() + 30 * [Diagnostics.Stopwatch]::Frequency) -Stdout "$run\task-probe.out" -Stderr "$run\task-probe.err" -Executable "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Arguments @('-NoProfile', '-File', $probe)
'== bounded child: ' + ($r | ConvertTo-Json -Compress)
Get-Content "$run\task-probe.out" -Raw
Get-Content "$run\task-probe.err" -Raw
'== direct'
& $probe -Seconds 3
