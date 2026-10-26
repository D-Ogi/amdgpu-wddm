param([Parameter(Mandatory)][string]$FixtureRoot, [string]$ModulePath = (Join-Path $PSScriptRoot 'BuildTree.psm1'))
$ErrorActionPreference = 'Stop'
if ($ModulePath) {
    $module = Import-Module $ModulePath -Force -PassThru -DisableNameChecking
} else {
    . (Join-Path $PSScriptRoot 'Remove-SharedBuildTree.ps1')
}
$base = Join-Path ([IO.Path]::GetFullPath($FixtureRoot)) ('shared-cleanup-test-' + [guid]::NewGuid().ToString('N'))
$repo = Join-Path $base 'repo'
$work = Join-Path $base 'work'
$tree = Join-Path $work 'shared'
New-Item -ItemType Directory -Path $repo, $work -Force | Out-Null
git init -q $repo
git -C $repo -c user.name=Test -c user.email=test@example.invalid commit -q --allow-empty -m seed
function Get-Checkout { return $repo }
function Invoke-Locked($Context, [scriptblock]$Action) { & $Action }
if ($ModulePath) {
    # Substitute only fixture repository discovery; exercise the module's real
    # cleanup implementation and lock wrapper rather than a copied function.
    & $module {
        param($FixtureRepo)
        $script:cleanupFixtureRepo = $FixtureRepo
        function script:Get-Checkout { return $script:cleanupFixtureRepo }
    } $repo
}
function Add-Tree {
    git -C $repo worktree add --detach $tree HEAD 2>&1 | Out-Null
    if ($LASTEXITCODE) { throw 'add failed' }
}
function Remove-Target { Remove-SharedBuildTree $null fixture HEAD $tree $work }
$passed = 0
Remove-Target; $passed++ # No tree and no registration.
Add-Tree
Remove-Target
Add-Tree; $passed++ # Reusable after normal cleanup.
Set-Content -LiteralPath (Join-Path $tree 'preserve.txt') -Value 'uncommitted'
$refused = $false
try { Remove-Target } catch { $refused = $true }
if (-not $refused -or -not (Test-Path (Join-Path $tree 'preserve.txt'))) { throw 'Dirty tree not preserved' }
$passed++
Remove-Item -LiteralPath (Join-Path $tree 'preserve.txt')
# Simulate the old runner losing the files while Git retains the entry.
if ([IO.Path]::GetFullPath($tree) -ne [IO.Path]::GetFullPath((Join-Path $work 'shared'))) { throw 'Boundary' }
Remove-Item -LiteralPath $tree -Recurse -Force
Remove-Target
Add-Tree; $passed++
Remove-Target
Add-Tree
git -C $repo worktree lock --reason active $tree
if ($LASTEXITCODE) { throw 'lock failed' }
$refused = $false
try { Remove-Target } catch { $refused = $true }
if (-not $refused -or -not (Test-Path $tree)) { throw 'Locked worktree not preserved' }
$passed++
git -C $repo worktree unlock $tree
Remove-Target
Add-Tree
git -C $repo worktree lock --reason active $tree
if ($LASTEXITCODE) { throw 'lock failed' }
if ([IO.Path]::GetFullPath($tree) -ne [IO.Path]::GetFullPath((Join-Path $work 'shared'))) { throw 'Boundary' }
Remove-Item -LiteralPath $tree -Recurse -Force
$refused = $false
try { Remove-Target } catch { $refused = $true }
# Git 2.27's porcelain listing omits lock annotations. Inspect this fixture's own lock file.
$lockFile = Join-Path $repo '.git/worktrees/shared/locked'
if (-not $refused -or -not (Test-Path -LiteralPath $lockFile) -or (Get-Content -Raw $lockFile).Trim() -ne 'active') { throw 'Missing locked worktree registration not preserved' }
$passed++
git -C $repo worktree unlock $tree
Remove-Target
New-Item -ItemType Directory -Path $tree | Out-Null
$refused = $false
try { Remove-Target } catch { $refused = $true }
if (-not $refused -or -not (Test-Path $tree)) { throw 'Unregistered directory not preserved' }
$passed++
$refused = $false
try { Remove-SharedBuildTree $null fixture HEAD $repo $work } catch { $refused = $true }
if (-not $refused -or -not (Test-Path $repo)) { throw 'Outside path not preserved' }
$passed++
[ordered]@{status='PASS'; cases=$passed; fixture=$base} | ConvertTo-Json

