#Requires -Version 7.0
# Release gate: no fix lost its comment on the way into this release (marker_guard.py next to this file).
#
#   pwsh -File tools\release\test-marker-guard.ps1 [-Repo <repository>] [-Head <revision>] [-Out <directory>]
#
# Every fix and optimisation in this repository leaves a comment with its id (BD-075, C50, K184, M779, E52, or a
# TODO/FIXME). A train merge or a rebase can drop such a comment together with the code it explains, and nothing
# else in the gate set notices: the host tests of the losing side went with it. This gate takes every anchored
# comment of every revision in marker-guard-sources.json and looks for it in the release head.
#
#   GONE      an id occurs fewer times in a file of the head than in that file of a source, and the file did not
#             just move. A fix or its explanation was dropped. This fails the gate.
#   REWORDED  the comment line is not in the head verbatim, but the id is still there as often as before: a
#             rewrap or a deliberate update. Printed, does not fail.
#   CHANGED   the comment is there and the code right after it matches no source's version: a merge result
#             nobody wrote. Printed, does not fail, and the release review reads it.
#
# A source is a commit, never a branch (marker-guard-sources.json says why). An id the head drops on purpose goes
# into tools\release\marker-guard-accept.txt, one line per decision: "<id> <path> <reason>".
#
# build-release.ps1 runs this gate on the repository it builds from. It needs git and python on the path and the
# source commits in that repository; without them it fails rather than pass quietly.
param(
    [string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Head = 'HEAD',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$table = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'marker-guard-sources.json') -Raw | ConvertFrom-Json
if (-not $table.sources -or @($table.sources).Count -eq 0) { throw 'marker-guard-sources.json names no source' }

# Every source commit must be in this repository, and the message says which one is not: a missing commit is the
# one way this gate can look green without having compared anything.
$missing = @(foreach ($s in $table.sources) {
    & git -C $Repo cat-file -e "$($s.commit)^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) { "$($s.name) ($($s.commit))" }
})
if ($missing.Count) { throw "marker guard: $Repo does not have $($missing -join ', ')" }

$argv = @((Join-Path $PSScriptRoot 'marker_guard.py'), '--repo', $Repo, '--head', $Head,
          '--baseline', [string][int]$table.baseline)
foreach ($s in $table.sources) { $argv += @('--source', [string]$s.commit) }
$argv += @('--paths') + @($table.paths | ForEach-Object { [string]$_ })
$accept = Join-Path $PSScriptRoot 'marker-guard-accept.txt'
if (Test-Path -LiteralPath $accept) { $argv += @('--accept', $accept) }
if ($Out) {
    $null = New-Item -ItemType Directory -Force $Out
    $argv += @('--json', (Join-Path $Out 'marker-guard.json'))
}

# The names beside the commits, so that a finding can be read without looking the commit up by hand.
'marker guard: head {0} in {1}' -f $Head, $Repo
foreach ($s in $table.sources) { '  source {0}  {1}' -f $s.commit.Substring(0, 8), $s.name }
& python @argv
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Error "marker guard: a fix lost its comment (GONE above). Restore it, or record the decision in $accept"; exit 1 }
'PASS: marker guard, no GONE anchor'
exit 0
