#Requires -Version 7.0
# The outputs of a package round of build-release-set.ps1 -Package, checked all at once before the first step (BD-272).
#
# Each step of a package round refuses an output of an earlier round in the same -From: the setup window
# (<From>\setup\<flavour>), the package (<From>\package\<flavour>), the submission CAB of the release flavour
# (<From>\submission\amdgpu-wddm-<version>.cab) and, with -Parallel, the worktree of each flavour
# (<From>\package-wt-<flavour>). Each refusal came only when its step started, minutes into the round, and one at a
# time (tester.34 r7: three runs, each stopped by the next output). Get-PackageRoundOutputs names every output that a
# round with these flavours would refuse; build-release-set.ps1 lists them in one message before any step.
# Move-PackageRoundOutputs moves them into <From>\rounds\<name>\ instead (-SetAside <name>), with the SHA-256 of every
# file before and after the move in <From>\rounds\<name>\set-aside.json. It never moves a worktree: git keeps its
# registration, so `git worktree remove` is the way to remove one.

function Get-PackageRoundOutputs([string]$From, [string[]]$Flavours, [string]$Version, [switch]$Parallel) {
    $list = [Collections.Generic.List[object]]::new()
    foreach ($flavour in $Flavours) {
        $list.Add([pscustomobject]@{ kind = 'setup'; flavour = $flavour; path = (Join-Path $From "setup\$flavour"); worktree = $false })
        $list.Add([pscustomobject]@{ kind = 'package'; flavour = $flavour; path = (Join-Path $From "package\$flavour"); worktree = $false })
        if ($flavour -eq 'release') {
            $list.Add([pscustomobject]@{ kind = 'submission CAB'; flavour = $flavour
                    path = (Join-Path $From "submission\amdgpu-wddm-$Version.cab"); worktree = $false })
        }
        if ($Parallel -and $Flavours.Count -gt 1) {
            $list.Add([pscustomobject]@{ kind = 'worktree'; flavour = $flavour; path = (Join-Path $From "package-wt-$flavour"); worktree = $true })
        }
    }
    return @($list | Where-Object { Test-Path -LiteralPath $_.path })
}

function Format-PackageRoundOutputs($Outputs) {
    $lines = @(foreach ($o in $Outputs) {
        "  $($o.kind) ($($o.flavour)): $($o.path)$(if ($o.worktree) { ' (remove it with git worktree remove)' })"
    })
    return "$(@($Outputs).Count) output(s) of an earlier package round exist, and the round refuses each of them:`n" +
        ($lines -join "`n") + "`nRemove them, or pass -SetAside <round name> to move them into <From>\rounds\<name>."
}

# Relative path -> SHA-256 of every file of a file or a directory.
function Get-OutputHashes([string]$Path) {
    $map = [ordered]@{}
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $map[[IO.Path]::GetFileName($Path)] = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        return $map
    }
    $base = [IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
    foreach ($f in Get-ChildItem -LiteralPath $Path -Recurse -File -Force | Sort-Object FullName) {
        $map[$f.FullName.Substring($base.Length).Replace('\', '/')] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    }
    return $map
}

function Test-SameHashes($A, $B) {
    if ($A.Count -ne $B.Count) { return $false }
    foreach ($k in $A.Keys) { if (-not $B.Contains($k) -or $B[$k] -ne $A[$k]) { return $false } }
    return $true
}

# Moves the outputs into <From>\rounds\<Name>\, each at its path relative to -From. Throws before it moves anything
# when an output is a worktree, the round directory already holds one of the paths, or the name is not a plain name.
# Returns the record that it writes to set-aside.json.
function Move-PackageRoundOutputs([string]$From, $Outputs, [string]$Name) {
    if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { throw "-SetAside takes a plain name (letters, digits, . _ -), not '$Name'" }
    $From = [IO.Path]::GetFullPath($From).TrimEnd('\')
    $round = Join-Path $From "rounds\$Name"
    $trees = @($Outputs | Where-Object { $_.worktree })
    if ($trees.Count) { throw "-SetAside does not move a worktree: $(($trees | ForEach-Object { $_.path }) -join ', ') (remove it with git worktree remove)" }
    $plan = @(foreach ($o in $Outputs) {
        $full = [IO.Path]::GetFullPath($o.path)
        $rel = $full.Substring($From.Length + 1)
        $dest = Join-Path $round $rel
        if (Test-Path -LiteralPath $dest) { throw "the round $round already holds $rel" }
        [pscustomobject]@{ output = $o; rel = $rel; dest = $dest; before = (Get-OutputHashes $full) }
    })
    $record = [ordered]@{ tool = 'build-release-set'; step = 'set-aside'; utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        from = $From; round = $round; outputs = @() }
    foreach ($p in $plan) {
        New-Item -ItemType Directory -Force (Split-Path $p.dest) | Out-Null
        Move-Item -LiteralPath $p.output.path -Destination $p.dest
        $after = Get-OutputHashes $p.dest
        if (-not (Test-SameHashes $p.before $after)) { throw "the files of $($p.rel) in $round do not have the SHA-256 they had before the move" }
        $record.outputs += [ordered]@{ kind = $p.output.kind; flavour = $p.output.flavour; path = $p.rel.Replace('\', '/')
            files = $p.before.Count; sha256 = $p.before }
    }
    New-Item -ItemType Directory -Force $round | Out-Null
    $file = Join-Path $round 'set-aside.json'
    $old = if (Test-Path -LiteralPath $file) { @(Get-Content -LiteralPath $file -Raw | ConvertFrom-Json -AsHashtable) } else { @() }
    [IO.File]::WriteAllText($file, (ConvertTo-Json -InputObject @($old + $record) -Depth 8))
    return $record
}

Export-ModuleMember -Function Get-PackageRoundOutputs, Format-PackageRoundOutputs, Get-OutputHashes, Move-PackageRoundOutputs
