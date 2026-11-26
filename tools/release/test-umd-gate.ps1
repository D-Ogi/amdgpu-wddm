# Gate umd-gate: the rules of tools\release\umd-gate.ps1 (Resolve-Bc250UmdGate), checked with fixtures. Each
# fixture is a small git repository with the two files that the user-mode gates read, a build directory with a
# build database, and a plan.json of a set that names a row and a commit. No Mesa, no release set, no network.
# The function must take the pair of the set's own build first, then a prepared umd-gate tree, and it must refuse
# a tree at another commit, a tree without the files, a tree without a build, and a set without a zink row.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }),
    [string]$Out = "$Root\scratch\umd-gate-test"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\umd-gate.ps1"
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out | Out-Null
$cases = 0; $bad = 0

# A git repository with the files the gates read. Returns the commit at HEAD.
function New-SourceFixture([string]$Path, [switch]$WithoutDeviceFile) {
    New-Item -ItemType Directory -Force (Join-Path $Path 'src\gallium\auxiliary\util') | Out-Null
    # Each fixture needs its own commit. Two repositories with the same content, the same author and the same
    # second give one commit, and then a case about one tree can take the other tree.
    Set-Content -LiteralPath (Join-Path $Path 'fixture.txt') -Value $Path -Encoding ascii
    Set-Content -LiteralPath (Join-Path $Path 'src\gallium\auxiliary\util\u_inlines.h') -Value '/* fixture */' -Encoding ascii
    if (-not $WithoutDeviceFile) {
        New-Item -ItemType Directory -Force (Join-Path $Path 'src\gallium\frontends\d3d10umd') | Out-Null
        Set-Content -LiteralPath (Join-Path $Path 'src\gallium\frontends\d3d10umd\Device.cpp') -Value '/* fixture */' -Encoding ascii
    }
    & git -C $Path init -q 2>&1 | Out-Null
    & git -C $Path config user.email 'umd-gate-test@example.com' | Out-Null
    & git -C $Path config user.name 'umd-gate test' | Out-Null
    & git -C $Path config commit.gpgsign false | Out-Null
    & git -C $Path add -A 2>&1 | Out-Null
    & git -C $Path commit -q -m 'fixture' 2>&1 | Out-Null
    return ((& git -C $Path rev-parse HEAD) | Out-String).Trim()
}

function New-BuildFixture([string]$Path) {
    New-Item -ItemType Directory -Force $Path | Out-Null
    Set-Content -LiteralPath (Join-Path $Path 'compile_commands.json') -Value '[]' -Encoding ascii
    return $Path
}

# A set whose plan.json holds one row. A built set holds its rows under kept, in the other shape.
function New-SetFixture([string]$Name, [hashtable]$Row, [switch]$Kept) {
    $dir = Join-Path $Out "set-$Name"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $plan = if ($Kept) { @{ builds = @(); kept = @($Row); carried = @() } } else { @{ builds = @($Row); kept = @(); carried = @() } }
    $plan | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'plan.json') -Encoding utf8NoBOM
    return $dir
}

# The row of a set that is built: the fields are in its definition, and the directories only in the command line
# of the recipe. A resolver that reads the planned fields alone finds no commit in such a set.
function KeptRow([string]$Id, [string]$Commit, [string]$Src, [string]$OutDir) {
    return @{ id = $Id; status = 'ok'
              command = "P:\x\tools\build\build-mesa.ps1 -Config zink-umd -Source $Src -Build $OutDir -Arch x64 -Flavour release"
              definition = @{ id = $Id; flavour = 'release'; arch = 'x64'; commit = $Commit
                              args = @('-Config', 'zink-umd', '-Arch', 'x64') } }
}

function Accept([string]$Name, [string]$Set, [string]$FakeRoot, [string]$Source, [string]$Build) {
    $script:cases++
    try {
        $gate = Resolve-Bc250UmdGate -Set $Set -Root $FakeRoot
        if ($gate.Source -ne $Source -or $gate.Build -ne $Build) {
            Write-Output "FAIL $Name : took $($gate.Source) + $($gate.Build)"; $script:bad++
        } else { Write-Output "took the pair as required: $Name ($($gate.From))" }
    } catch { Write-Output "FAIL $Name : refused: $($_.Exception.Message)"; $script:bad++ }
}

function Refuse([string]$Name, [string]$Set, [string]$FakeRoot, [string]$Reason) {
    $script:cases++
    try {
        $gate = Resolve-Bc250UmdGate -Set $Set -Root $FakeRoot
        Write-Output "FAIL $Name : took $($gate.Source) + $($gate.Build)"; $script:bad++
    } catch {
        if ($_.Exception.Message -match $Reason) { Write-Output "refused as required: $Name" }
        else { Write-Output "FAIL $Name : refused for another reason: $($_.Exception.Message)"; $script:bad++ }
    }
}

# The fake workspace: a prepared umd-gate tree with a build, one without a build, and one without the file the
# analysis gate reads.
$fake = Join-Path $Out 'root'
$goodDir = Join-Path $fake 'scratch\t99\release\umd-gate-good'
$good = New-SourceFixture (Join-Path $goodDir 'src')
$goodBuild = New-BuildFixture (Join-Path $goodDir 'zink-x64')
$nobuildDir = Join-Path $fake 'scratch\t99\release\umd-gate-nobuild'
$nobuild = New-SourceFixture (Join-Path $nobuildDir 'src')
$partialDir = Join-Path $fake 'scratch\t99\release\umd-gate-partial'
$partial = New-SourceFixture (Join-Path $partialDir 'src') -WithoutDeviceFile
[void](New-BuildFixture (Join-Path $partialDir 'zink-x64'))
# The pair of a set that is built: its own directories for the row.
$setBuiltSrc = Join-Path $Out 'built\s'
$built = New-SourceFixture $setBuiltSrc
$setBuiltOut = New-BuildFixture (Join-Path $Out 'built\o')

function Row([string]$Id, [string]$Commit, [string]$Src, [string]$OutDir, [string[]]$Arguments) {
    $row = @{ id = $Id; flavour = 'release'; arch = 'x64'; commit = $Commit
              args = $(if ($Arguments) { $Arguments } else { @('-Config', 'zink-umd', '-Arch', 'x64') }) }
    if ($Src) { $row['dirs'] = @{ src = $Src; out = $OutDir } }
    return $row
}

Accept 'the built set wins' (New-SetFixture 'built' (Row 'zink-x64-rel' $built $setBuiltSrc $setBuiltOut)) $fake $setBuiltSrc $setBuiltOut
Accept 'a prepared tree when the set is not built' `
    (New-SetFixture 'tree' (Row 'zink-x64-rel' $good (Join-Path $Out 'absent\s') (Join-Path $Out 'absent\o'))) `
    $fake (Join-Path $goodDir 'src') $goodBuild
Accept 'the row of a built set, with the directories in its command line' `
    (New-SetFixture 'kept' (KeptRow 'zink-x64-rel' $built $setBuiltSrc $setBuiltOut) -Kept) $fake $setBuiltSrc $setBuiltOut
Accept 'the row found by its arguments' `
    (New-SetFixture 'byargs' (Row 'desktop-zink-release' $good '' '' @('-Config', 'zink-umd', '-Arch', 'x64'))) `
    $fake (Join-Path $goodDir 'src') $goodBuild
Refuse 'a commit that no tree holds' (New-SetFixture 'other' (Row 'zink-x64-rel' ('0' * 40) '' '')) $fake ('0' * 40)
Refuse 'a tree without a build' (New-SetFixture 'nobuild' (Row 'zink-x64-rel' $nobuild '' '')) $fake 'no umd-gate tree'
Refuse 'a tree without the file of the analysis gate' (New-SetFixture 'partial' (Row 'zink-x64-rel' $partial '' '')) $fake 'no umd-gate tree'
Refuse 'a set with no zink row' `
    (New-SetFixture 'nozink' (Row 'kmd-rel' $good '' '' @('-Flavour', 'release'))) $fake 'no release zink-umd row'
Refuse 'a set with no plan' (Join-Path $Out 'set-absent') $fake 'no plan.json'

Write-Output "umd-gate: $cases cases, $bad failures"
exit $(if ($bad) { 1 } else { 0 })
