#Requires -Version 7.0
# Release gate (BD-204 step 2): no fix marker is on one RADV WDDM line of the release and missing on the other.
#
#   pwsh -File tools\release\test-winsys-lines.ps1 [-Sources <release-sources.json>] [-Root <BC250_ROOT>] [-Out <directory>]
#
# The release builds its Vulkan ICDs from more than one Mesa line. A fix that reaches one line and not the other is
# invisible to the marker guard (test-marker-guard.ps1), which follows one line. marker_guard.py lines reads the
# commit that -Sources pins for each line of winsys-lines.json, takes every BD id in a comment under the config's
# paths and in the subjects of the commits since the merge base of the pair, and fails on a marker that one line has
# and the other does not, unless an entry of winsys-lines.json says why (contract, line-only, equivalent with the
# commits of the other line, or open drift). An entry that no longer matches a difference fails as stale. The named
# fixes of 'both_lines' must be on every line.
#
# The Mesa checkout is the first checkout of the repository in -Sources that holds every line. Without one the gate
# fails and names the commits to fetch: it never passes without having compared the lines.
param(
    [string]$Sources = (Join-Path $PSScriptRoot 'release-sources.json'),
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$argv = @((Join-Path $PSScriptRoot 'marker_guard.py'), 'lines', '--config', (Join-Path $PSScriptRoot 'winsys-lines.json'),
          '--sources', $Sources, '--root', $Root)
if ($Out) {
    $null = New-Item -ItemType Directory -Force $Out
    $argv += @('--json', (Join-Path $Out 'winsys-lines.json'))
}
& python -B @argv
if ($LASTEXITCODE -ne 0) { Write-Error 'both lines: FAIL (see above). Port the fix, or record in winsys-lines.json why the other line does without it'; exit 1 }
'PASS: both lines, no unaccepted fix marker on one line only'
exit 0
