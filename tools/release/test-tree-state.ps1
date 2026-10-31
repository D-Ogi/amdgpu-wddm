#Requires -Version 7.0
# Host test of TreeState.psm1 (BD-263) on a git repository of its own, and of its calls in build-release.ps1: the
# release gates must read the commit that the package names, so a dirty tree is refused, a lab candidate's dirty
# tree is recorded, and any change of the tree during the run stops the build.
#   pwsh -File tools\release\test-tree-state.ps1 -Out <dir>     (the 'tree-state' gate of quick.ps1)
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'TreeState.psm1') -Force
$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('tree-state-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$repo = Join-Path $base 'repo'
$scratch = Join-Path $base 'scratch'
$passed = 0
function Assert([bool]$Ok, [string]$What) { if (-not $Ok) { throw "FAIL: $What" }; $script:passed++ }
function Get-Error([scriptblock]$Block) { try { & $Block; return '' } catch { return $_.Exception.Message } }
function Invoke-TestGit { & git.exe -C $repo @args 2>&1 | Out-Null; if ($LASTEXITCODE) { throw "git $args failed" } }

New-Item -ItemType Directory -Force $repo | Out-Null
Invoke-TestGit init -q
Invoke-TestGit config user.name test
Invoke-TestGit config user.email research@example.com
Invoke-TestGit config core.autocrlf false
[IO.File]::WriteAllText((Join-Path $repo 'gate.ps1'), "'the committed gate'`n")
[IO.File]::WriteAllText((Join-Path $repo '.gitignore'), "build/`n")
Invoke-TestGit add -A
Invoke-TestGit commit -q -m one

# A clean tree.
$s = Get-TreeState $repo $scratch
Assert ($s.clean -and $s.head -match '^[0-9a-f]{40}$' -and -not $s.entries.Count -and -not $s.digest) 'a clean tree'
Assert ((Get-Error { Assert-TreeClean $s '' }) -eq '') 'a clean tree passes'
# A build product under an ignored folder is not an edit.
New-Item -ItemType Directory -Force (Join-Path $repo 'build') | Out-Null
[IO.File]::WriteAllText((Join-Path $repo 'build\out.obj'), 'object')
Assert ((Get-Error { $null = Assert-TreeUnchanged $s $scratch 'test' }) -eq '') 'an ignored build product is no change'

# An uncommitted edit: refused, unless the build reads a work tree on purpose.
[IO.File]::WriteAllText((Join-Path $repo 'gate.ps1'), "'an edit of another lane'`n")
$d = Get-TreeState $repo $scratch
Assert (-not $d.clean -and $d.entries.Count -eq 1 -and $d.entries[0] -match 'gate\.ps1' -and $d.digest) 'an edited tree'
$e = Get-Error { Assert-TreeClean $d '' }
Assert ($e -match 'is not clean \(1 uncommitted entries' -and $e -match 'gate\.ps1' -and $e -match 'BD-263') "a dirty tree is refused: $e"
Assert ((Get-Error { Assert-TreeClean $d 'lab candidate before its commit' }) -eq '') 'a lab candidate reads its work tree'
Assert (@(Get-ChildItem -LiteralPath $scratch -Force -ErrorAction SilentlyContinue).Count -eq 0) 'no temporary file is left'

# Changes during the run.
$e = Get-Error { $null = Assert-TreeUnchanged $s $scratch 'during the quality suite' }
Assert ($e -match 'changed during the run \(during the quality suite\)' -and $e -match 'new: .*gate\.ps1') "an edit after the start: $e"
Assert ((Get-Error { $null = Assert-TreeUnchanged $d $scratch 'x' }) -eq '') 'the same dirty tree is no change'
[IO.File]::WriteAllText((Join-Path $repo 'gate.ps1'), "'a second edit of the same file'`n")
$e = Get-Error { $null = Assert-TreeUnchanged $d $scratch 'x' }
Assert ($e -match 'the same entries with other content') "a second edit of a dirty file: $e"
[IO.File]::WriteAllText((Join-Path $repo 'gate.ps1'), "'the committed gate'`n")
Assert ((Get-Error { $null = Assert-TreeUnchanged $s $scratch 'x' }) -eq '') 'an edit taken back'
[IO.File]::WriteAllText((Join-Path $repo 'new-gate.ps1'), 'new')
$e = Get-Error { $null = Assert-TreeUnchanged $s $scratch 'x' }
Assert ($e -match 'new: \?\? new-gate\.ps1') "a new untracked file: $e"
$u1 = Get-TreeState $repo $scratch
[IO.File]::WriteAllText((Join-Path $repo 'new-gate.ps1'), 'other')
$e = Get-Error { $null = Assert-TreeUnchanged $u1 $scratch 'x' }
Assert ($e -match 'other content') "new content of an untracked file: $e"
Invoke-TestGit add -A
Invoke-TestGit commit -q -m two
$e = Get-Error { $null = Assert-TreeUnchanged $s $scratch 'x' }
Assert ($e -match 'HEAD moved from ' + $s.head.Substring(0, 8)) "a commit during the run: $e"
$e = Get-Error { Get-TreeState (Join-Path $base 'no-repo') $scratch }
Assert ($e -match 'failed') 'a folder that is not a git tree'

# build-release.ps1: the state is taken before the first gate, refused when dirty (except -NoProvenance), checked
# before and after the quality suite, and the suite's own exit decides only after that check.
$text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build-release.ps1'))
$at = { param($s) $text.IndexOf($s) }
$start = & $at '$treeAtStart = Get-TreeState $repo $treeScratch'
$clean = & $at 'Assert-TreeClean $treeAtStart $(if ($NoProvenance) { $NoProvenanceReason })'
$firstGate = & $at "'commits',"
$before = & $at "Assert-TreeUnchanged `$treeAtStart `$treeScratch 'before the quality suite'"
$suite = & $at "& `$qualityCmd `$Root `$qualityOut `$repo 'Full'"
$after = & $at "Assert-TreeUnchanged `$treeAtStart `$treeScratch 'during the quality suite'"
$exit = & $at "if (`$qualityExit -ne 0) { throw 'Full uncached release quality gates failed' }"
Assert ($start -gt 0 -and $clean -gt $start -and $firstGate -gt $clean) 'build-release.ps1 takes and checks the tree before its first gate'
Assert ($before -gt $firstGate -and $suite -gt $before -and $after -gt $suite -and $exit -gt $after) 'build-release.ps1 checks the tree around the quality suite'
Assert ($text.IndexOf("Join-Path `$Out 'quality-tree.json'") -gt $after) 'build-release.ps1 records the gated tree'

Remove-Item -LiteralPath $base -Recurse -Force
"PASS: recipe tree state, $passed checks (BD-263)"
