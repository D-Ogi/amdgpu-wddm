# Offline gate of the host watch and the kill switch. It touches no kd.exe and kills nothing but its own children.
#   pwsh -NoProfile -File tools\win\hostwatch\selftest.ps1 [-Out <dir>]
# Four checks:
#   parse        all three scripts parse with the Windows PowerShell 5.1 parser (no execution)
#   trigger      killswitch.ps1 with an impossible limit and a process name that cannot exist: the trigger fires,
#                the dry run kills nothing, and a stale stop file is cleared at start instead of ending the run
#   stop         the run ends after the stop file appears again
#   sample       hostwatch.ps1 writes nonpaged-pool lines at the interval it was given
# The live validation of the real kill path is killswitch-dryrun.ps1, which is run by hand because it kills every
# ping.exe on this PC.
# BC250_ROOT is the workspace root; by default the parent directory of this repository.
param(
    [string]$Out = (Join-Path (& { if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path } }) 'scratch\hostwatch\selftest')
)
$ErrorActionPreference = 'Stop'
$failed = @()
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { "PASS $name" } else { $script:failed += $name; "FAIL $name - $detail" }
}

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -File -ErrorAction SilentlyContinue | Remove-Item -Force
$ps51 = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$scripts = 'hostwatch.ps1', 'killswitch.ps1', 'killswitch-dryrun.ps1', 'selftest.ps1'

$errors = @()
foreach ($name in $scripts) {
    $tokens = $null; $parse = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$parse)
    if ($parse.Count) { $errors += "$name line $($parse[0].Extent.StartLineNumber): $($parse[0].Message)" }
}
Check 'parse' ($errors.Count -eq 0) ($errors -join '; ')

# A process name no installer can produce, so the kill list is always empty even without -DryRun.
$victim = 'bc250-hostwatch-selftest-absent'
$log = Join-Path $Out 'killswitch.log'
$stop = Join-Path $Out 'killswitch.stop'
New-Item -ItemType File -Path $stop -Force | Out-Null   # stale: the run must clear it and keep watching
$argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'killswitch.ps1'),
          '-GrowthLimitMB', '-1', '-ProcessName', $victim, '-Log', $log, '-StopFile', $stop, '-DryRun')
$proc = Start-Process -FilePath $ps51 -ArgumentList $argv -WindowStyle Hidden -PassThru
# The first Win32_PerfFormattedData_PerfOS_Memory query of a session can take several seconds, so wait for the
# evidence instead of a fixed sleep.
$text = ''
for ($i = 0; $i -lt 60 -and -not ($text -match 'dry run, nothing killed'); $i++) {
    Start-Sleep -Milliseconds 500
    $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
}
$running = -not $proc.HasExited
$fired = ($text -match 'KILL .*over limit') -and ($text -match 'dry run, nothing killed')
Check 'trigger' ($running -and $fired) "still running=$running, log=$($text -replace '\s+', ' ')"

New-Item -ItemType File -Path $stop -Force | Out-Null
$exited = $proc.WaitForExit(20000)
if (-not $exited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
$text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
Check 'stop' ($exited -and $text -match 'stop \(stop file seen\)') "exited=$exited"

$hwLog = Join-Path $Out 'hostwatch.log'
$hw = Start-Process -FilePath $ps51 -WindowStyle Hidden -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'hostwatch.ps1'),
    '-Log', $hwLog, '-Every', '1')
$lines = @()
for ($i = 0; $i -lt 60 -and $lines.Count -lt 2; $i++) {
    Start-Sleep -Milliseconds 500
    $lines = @(if (Test-Path $hwLog) { Get-Content $hwLog | Where-Object { $_ -match 'npp=\d' } })
}
Stop-Process -Id $hw.Id -Force -ErrorAction SilentlyContinue
Check 'sample' ($lines.Count -ge 2) "$($lines.Count) sample line(s)"

if ($failed.Count) { "FAILED: $($failed -join ', ')"; exit 1 }
'PASS hostwatch selftest (4 checks)'
exit 0
