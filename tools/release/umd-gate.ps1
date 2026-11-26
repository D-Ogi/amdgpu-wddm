# Finds the umd-gate tree of a release set: the Mesa tree at the commit of the set's zink-umd row, and the
# configured build of that tree. Three gates of tools\quality\quick.ps1 (umd-contract, surface-control,
# umd-analysis) read those two directories through BC250_UMD_SOURCE and BC250_UMD_BUILD, and the KMD recipe
# refuses its gates when the two variables are absent. build-release-set.ps1 -Package gets the pair from its
# -UmdSource and -UmdBuild arguments. A tool that builds one row out of a set, such as the fast-build script of
# the lab kit, has no such argument, and this is where it gets the pair.
#
# Dot-source the file and call the function:
#
#   . tools\release\umd-gate.ps1
#   $gate = Resolve-Bc250UmdGate -Set P:\BC-250\scratch\t39\rc5-set
#   $env:BC250_UMD_SOURCE = $gate.Source; $env:BC250_UMD_BUILD = $gate.Build
#
# The function refuses when it knows no tree at the commit of the row. It never takes a tree at another commit:
# a build of the KMD against the gates of another user-mode tree proves nothing.

# The files the three gates read. A tree without them is not a umd-gate tree, whatever its commit is.
$script:Bc250UmdGateSourceFiles = @('src\gallium\auxiliary\util\u_inlines.h', 'src\gallium\frontends\d3d10umd\Device.cpp')
$script:Bc250UmdGateBuildFiles = @('compile_commands.json')

function Test-Bc250UmdGateTree([string]$Source, [string]$Build, [string]$Commit) {
    foreach ($f in $script:Bc250UmdGateSourceFiles) { if (-not (Test-Path -LiteralPath (Join-Path $Source $f))) { return $false } }
    foreach ($f in $script:Bc250UmdGateBuildFiles) { if (-not (Test-Path -LiteralPath (Join-Path $Build $f))) { return $false } }
    $head = (& git -C $Source rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { return $false }
    return ([string]$head).Trim() -ceq $Commit
}

# The build directory of a prepared tree: a directory beside the source that holds a build database. The name of
# the row's configuration and its architecture come first, because a tree can hold the build of more than one.
function Find-Bc250UmdGateBuild([string]$Directory, [string]$Arch) {
    $all = @(Get-ChildItem -LiteralPath $Directory -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'compile_commands.json') })
    $pick = @($all | Where-Object { $_.Name -like "*zink*$Arch*" }) + @($all | Where-Object { $_.Name -like "*$Arch*" }) + $all
    if ($pick.Count) { return $pick[0].FullName }
    return ''
}

# One row of a plan, in the two shapes a plan holds. A row that the set planned carries its fields and its
# directories itself. A row that the set built or kept carries them in its definition, and the directories only
# in the command line of the recipe (-Source and -Build).
function ConvertTo-Bc250UmdGateRow($Row) {
    $d = if ($Row.definition) { $Row.definition } else { $Row }
    $src = ''; $out = ''
    if ($Row.dirs -and $Row.dirs.src) { $src = [string]$Row.dirs.src; $out = [string]$Row.dirs.out }
    elseif ($d.dirs -and $d.dirs.src) { $src = [string]$d.dirs.src; $out = [string]$d.dirs.out }
    elseif ($Row.command) {
        $m = [regex]::Match([string]$Row.command, '-Source\s+(?<src>\S+)\s+-Build\s+(?<out>\S+)')
        if ($m.Success) { $src = $m.Groups['src'].Value; $out = $m.Groups['out'].Value }
    }
    return [pscustomobject]@{
        Id = [string]$(if ($Row.id) { $Row.id } else { $d.id })
        Flavour = [string]$d.flavour; Arch = [string]$d.arch; Commit = [string]$d.commit
        Args = @($d.args | ForEach-Object { [string]$_ }); Source = $src; Build = $out
    }
}

function Resolve-Bc250UmdGate {
    param(
        [Parameter(Mandatory)][string]$Set,
        [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }),
        [string]$RowId = ''
    )
    $planPath = Join-Path $Set 'plan.json'
    if (-not (Test-Path -LiteralPath $planPath)) { throw "no plan.json in the set $Set" }
    $plan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
    $rows = @(@($plan.builds) + @($plan.kept) + @($plan.carried) | Where-Object { $_ } |
        ForEach-Object { ConvertTo-Bc250UmdGateRow $_ } | Where-Object { $_.Id })

    # The row: the release zink-umd row for x64, by its identifier or by its own arguments. The gates read a
    # release build, and the x64 build is the one the desktop and the D3D10 front end use.
    $zink = if ($RowId) { @($rows | Where-Object { $_.Id -ceq $RowId }) } else {
        @($rows | Where-Object { $_.Id -ceq 'zink-x64-rel' }) +
        @($rows | Where-Object {
            $_.Flavour -ceq 'release' -and $_.Arch -ceq 'x64' -and
            ($_.Args -contains 'zink-umd') -and ($_.Args -contains '-Config')
        })
    }
    $zink = @($zink | Where-Object { $_.Commit })
    if (-not $zink.Count) { throw "the set $Set has no release zink-umd row for x64, so there is no umd-gate commit" }
    $row = $zink[0]
    $commit = $row.Commit
    $arch = if ($row.Arch) { $row.Arch } else { 'x64' }
    $looked = [Collections.Generic.List[string]]::new()

    # 1. The directories the set itself uses for that row. When the set is built, they hold the tree at the
    #    commit and its build database, which is the pair the release gates themselves read.
    if ($row.Source -and $row.Build) {
        $looked.Add($row.Source)
        if (Test-Bc250UmdGateTree $row.Source $row.Build $commit) {
            return [pscustomobject]@{ Source = $row.Source; Build = $row.Build
                                      Commit = $commit; Row = $row.Id; From = 'the build of the set' }
        }
    }

    # 2. A prepared umd-gate tree. The release procedure makes one for each candidate under a release directory
    #    of the campaign, with the source in src and the build beside it.
    $patterns = @("$Root\scratch\*\release\umd-gate-*", "$Root\scratch\*\umd-gate-*", "$Root\scratch\umd-gate-*")
    foreach ($pattern in $patterns) {
        foreach ($dir in @(Get-Item -Path $pattern -ErrorAction SilentlyContinue | Where-Object { $_.PSIsContainer })) {
            $source = Join-Path $dir.FullName 'src'
            $looked.Add($source)
            $build = Find-Bc250UmdGateBuild $dir.FullName $arch
            if ($build -and (Test-Bc250UmdGateTree $source $build $commit)) {
                return [pscustomobject]@{ Source = $source; Build = $build
                                          Commit = $commit; Row = $row.Id; From = 'a prepared umd-gate tree' }
            }
        }
    }

    throw ("no umd-gate tree for $($row.Id) at commit ${commit}: a tree must hold " +
        "$($script:Bc250UmdGateSourceFiles -join ', ') with that commit at HEAD, and a build directory with " +
        "compile_commands.json. Looked at: $(if ($looked.Count) { $looked -join ', ' } else { 'nothing' }). " +
        'Make the tree as the release procedure does, or set BC250_UMD_SOURCE and BC250_UMD_BUILD yourself.')
}
