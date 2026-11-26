# Gate overlay-group-rerun: run_dcn_overlay.ps1 must give the same result when it runs again in the output
# directory of an earlier run. The quality suite keeps one output directory for a work tree (the build playbook
# says to keep the same -Out), so a runner that refuses a directory that it made itself fails the second build of
# that tree with no defect in the tree. The three cases are: a run in an empty directory (the positive control),
# the same run again in the same directory, and a run that must replace the content of the old directory instead
# of reading it.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\overlay-group-rerun"
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run_dcn_overlay.ps1'
$pwsh = (Get-Process -Id $PID).Path
$work = Join-Path $Out 'gate'
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force $Out, $work | Out-Null
$cases = 0; $bad = 0; $ok = $false

function Invoke-Runner([string]$Name) {
    $script:cases++
    $script:ok = $false
    $log = Join-Path $Out "$Name.log"
    & $pwsh -NoProfile -NonInteractive -File $runner -Root $Root -Out $work *> $log
    $code = $LASTEXITCODE
    $text = Get-Content -LiteralPath $log -Raw
    if ($code -ne 0) {
        $line = @($text -split "`n" | Where-Object { $_ -match 'Retained|Exception|FAIL|refused' } | Select-Object -First 1)
        Write-Output "FAIL $Name : exit $code ($(([string]$line).Trim()))"
        $script:bad++
        return
    }
    if ($text -notmatch 'overlay group: \d+ checks, 0 failures') {
        Write-Output "FAIL $Name : exit 0 without the summary of the overlay group"
        $script:bad++
        return
    }
    Write-Output "passed as required: $Name"
    $script:ok = $true
}

Invoke-Runner 'first-run'
Invoke-Runner 'second-run-same-out'

# The run must build its directory again. A file that only this test writes must not survive the run, because a
# directory that stays holds the object files and the executable of the earlier run.
$stale = Join-Path $work 'tree\stale-marker.txt'
Set-Content -LiteralPath $stale -Value 'from an earlier run' -Encoding ascii
Invoke-Runner 'third-run-with-stale-content'
if ($ok) {
    if (Test-Path -LiteralPath $stale) { Write-Output 'FAIL stale content: the marker of the earlier run is still there'; $bad++ }
    else { Write-Output 'passed as required: stale content replaced' }
}

Write-Output "overlay-group rerun: $cases cases, $bad failures"
exit $(if ($bad) { 1 } else { 0 })
