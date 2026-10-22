#Requires -Version 7.0
# The build-tree machinery that rebuild-check.ps1 and build-release-set.ps1 share: a tree of a commit with its submodules
# from the checkout's own module repositories (no network), the listed paths of a pinned input, and the argument
# placeholders of a release-sources.json recipe. Both tools call these functions; neither keeps a copy of them.
#
#   $ctx = New-BuildContext -Sources <json with "repositories"> -Root <BC250_ROOT> -Repo <recipe repository checkout>
#          [-Tools @{name = path}] [-SourceOf @{payload path = file}] [-Lock <mutex name>]
#
# The context holds the trees that New-Tree made, so Remove-Trees takes them away again. With -Lock, every git
# command that changes the administration of a shared checkout (worktree add, remove and prune) holds that named
# mutex, so parallel builds of one checkout do not race; the checkout of the files runs outside it.

function New-BuildContext {
    param(
        [Parameter(Mandatory)][string]$Sources,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Repo,
        [hashtable]$Tools = @{},
        [hashtable]$SourceOf = @{},
        [string]$Lock = ''
    )
    $t = @{}
    foreach ($k in $Tools.Keys) { $t[$k.ToLowerInvariant()] = $Tools[$k] }
    return [pscustomobject]@{
        Sources    = $Sources
        Root       = $Root
        Repo       = $Repo
        Provenance = Join-Path $PSScriptRoot 'provenance.py'
        Tools      = $t
        SourceOf   = $SourceOf
        Lock       = $Lock
        Trees      = [Collections.Generic.List[object]]::new()
    }
}

# Runs the script block while it holds the context's mutex (no mutex: at once). A mutex that a dead process left
# behind is taken over: the administration it guarded is git's own, and git keeps it consistent.
function Invoke-Locked($Context, [scriptblock]$Action) {
    if (-not $Context.Lock) { return & $Action }
    $mutex = [Threading.Mutex]::new($false, $Context.Lock)
    try {
        try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] { }
        return & $Action
    } finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

function Get-Checkout($Context, [string]$Name, [string]$Commit) {
    $out = & python $Context.Provenance locate --sources $Context.Sources --root $Context.Root --repo $Context.Repo $Name $Commit 2>&1
    if ($LASTEXITCODE -ne 0) { throw "locate ${Name} ${Commit}: $out" }
    return ($out | Select-Object -Last 1).Trim()
}

# A detached work tree of the commit, and its submodules from the module repositories of the checkout. The URL of
# each submodule is overridden on the command line only (-c), so no configuration of the checkout changes.
#
# -Clone makes a new repository instead: the history of the commit and the tags of the checkout, fetched from the
# checkout, then checked out with the checkout's own core.autocrlf and core.eol. A build that names its version with
# `git describe` gets the abbreviation of a clone of the fork that way, not the longer one of a shared checkout that
# holds every line of the fork (the DXVK engine reads v3.1.1-119-gc271e0e0 in a clone and v3.1.1-119-gc271e0e00 in
# scratch\forksync\dxvk, and that string is in the image).
function New-Tree($Context, [string]$Name, [string]$Commit, [string]$Dest, [switch]$Clone) {
    $checkout = Get-Checkout $Context $Name $Commit
    $common = (& git -C $checkout rev-parse --git-common-dir).Trim()
    if (-not [IO.Path]::IsPathRooted($common)) { $common = Join-Path $checkout $common }
    if ($Clone) {
        & git init -q $Dest 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git init $Dest failed" }
        $Context.Trees.Add([pscustomobject]@{ checkout = $null; tree = $Dest })
        foreach ($key in 'core.autocrlf', 'core.eol') {
            $value = & git -C $checkout config --local --get $key
            if ($LASTEXITCODE -eq 0 -and $value) { & git -C $Dest config $key $value.Trim() }
        }
        & git -C $Dest fetch -q --no-tags $checkout $Commit 'refs/tags/*:refs/tags/*' 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git fetch $Commit from $checkout into $Dest failed" }
        & git -C $Dest -c advice.detachedHead=false checkout -q --detach $Commit 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git checkout $Commit in $Dest failed" }
    } elseif ($Context.Lock) {
        # Only the administration of the shared checkout holds the mutex; the files are checked out after it.
        Invoke-Locked $Context {
            & git -C $checkout worktree add --no-checkout --detach $Dest $Commit 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "git worktree add $Dest $Commit in $checkout failed" }
        }
        $Context.Trees.Add([pscustomobject]@{ checkout = $checkout; tree = $Dest })
        & git -C $Dest reset -q --hard 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git reset --hard in $Dest failed" }
    } else {
        & git -C $checkout worktree add --detach $Dest $Commit 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git worktree add $Dest $Commit in $checkout failed" }
        $Context.Trees.Add([pscustomobject]@{ checkout = $checkout; tree = $Dest })
    }
    Initialize-Submodules $Dest (Join-Path $common 'modules')
}

function Initialize-Submodules([string]$Tree, [string]$ModuleBase) {
    if (-not (Test-Path -LiteralPath (Join-Path $Tree '.gitmodules'))) { return }
    $lines = @(& git -C $Tree config -f .gitmodules --get-regexp '^submodule\..*\.path$')
    foreach ($line in $lines) {
        if ($line -notmatch '^submodule\.(.+)\.path\s+(.+)$') { continue }
        $name = $Matches[1]; $sub = $Matches[2].Trim()
        $module = Join-Path $ModuleBase ($name -replace '/', '\')
        if (-not (Test-Path -LiteralPath $module)) { throw "submodule $name of $Tree is not initialised in the checkout ($module): run git submodule update --init there once" }
        & git -C $Tree -c protocol.file.allow=always -c "submodule.$name.url=$module" submodule update --init --quiet -- $sub 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git submodule update $sub in $Tree failed" }
        Initialize-Submodules (Join-Path $Tree $sub) (Join-Path $module 'modules')
    }
}

# The listed paths of a commit, as plain files (an input is read, never built in place). The export is the committed
# bytes: core.autocrlf=false, because the checkout's own setting must not change an input. A checkout with
# core.autocrlf=true gave the CRLF form of a header whose LF blob a recipe pins (the engine header of the D3D12 shell,
# train b30), so the same commit gave two inputs on two checkouts.
function Export-Input($Context, [string]$Name, [string]$Commit, [string[]]$Paths, [string]$Dest) {
    $checkout = Get-Checkout $Context $Name $Commit
    New-Item -ItemType Directory -Force $Dest | Out-Null
    $tar = Join-Path $Dest '..\input.tar'
    & git -C $checkout -c core.autocrlf=false archive --format=tar -o $tar $Commit @Paths
    if ($LASTEXITCODE -ne 0) { throw "git archive $Commit $($Paths -join ' ') in $checkout failed" }
    & "$env:SystemRoot\System32\tar.exe" -x -f $tar -C $Dest
    if ($LASTEXITCODE -ne 0) { throw "tar -x $tar failed" }
    Remove-Item -LiteralPath $tar -Force
}

# The inputs of one build, as release-sources.json names them: an inline JSON object becomes a file, a repository
# with paths becomes those paths of its commit, a repository without paths a tree of its commit, and a workspace
# path is read where it is. Returns name -> path; with -NoCreate the paths only (a plan builds nothing).
function Resolve-Inputs($Context, $Inputs, [string]$Base, [switch]$NoCreate) {
    $map = @{}
    if (-not $Inputs) { return $map }
    foreach ($n in $Inputs.Keys) {
        $spec = $Inputs[$n]
        $map[$n] = if ($spec.inline) { Join-Path $Base "$n.json" } elseif ($spec.workspace) { Join-Path $Context.Root ($spec.workspace -replace '/', '\') } else { Join-Path $Base $n }
        if ($NoCreate) { continue }
        if ($spec.inline) {
            New-Item -ItemType Directory -Force $Base | Out-Null
            [IO.File]::WriteAllText($map[$n], ($spec.inline | ConvertTo-Json -Depth 10))
        } elseif ($spec.repo -and $spec.paths) {
            Export-Input $Context $spec.repo $spec.commit @($spec.paths) $map[$n]
        } elseif ($spec.repo) {
            New-Tree $Context $spec.repo $spec.commit $map[$n]
        }
    }
    return $map
}

function Resolve-ToolPath($Context, [string]$Name) {
    $k = $Name.ToLowerInvariant()
    if ($Context.Tools.ContainsKey($k)) { return $Context.Tools[$k] }
    $envValue = [Environment]::GetEnvironmentVariable("BC250_TOOL_$($Name.ToUpperInvariant())")
    if ($envValue) { return $envValue }
    $c = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { throw "tool ${Name}: pass -Tool $Name=<path> or set BC250_TOOL_$($Name.ToUpperInvariant())" }
    return $c.Source
}

# An argument that starts with a placeholder is a path: its / become \. Other arguments keep their text (a switch such
# as /arch:AVX2 stays a switch). The map gives {src} {out} {root} and any other plain name ({tmp}, {jobs},
# {flavour}, {testout}, {recipe}), and 'inputs' gives {input:<name>}. {payload:<package path>} is the context's
# SourceOf, {tool:<name>} Resolve-ToolPath. {version} is refused while it is empty: a build that stamps a version
# resource must be told which release it is for.
function Expand-Arg($Context, [string]$Arg, [hashtable]$Map) {
    $isPath = $Arg.StartsWith('{')
    $value = [regex]::Replace($Arg, '\{([a-z_]+)(?::([^}]+))?\}', {
            param($m)
            $kind = $m.Groups[1].Value; $name = $m.Groups[2].Value
            switch ($kind) {
                'input' { if (-not $Map.inputs.ContainsKey($name)) { throw "unknown input $name" }; return $Map.inputs[$name] }
                'payload' { if (-not $Context.SourceOf.ContainsKey($name)) { throw "unknown payload $name" }; return $Context.SourceOf[$name] }
                'tool' { return (Resolve-ToolPath $Context $name) }
                'version' { if (-not $Map.version) { throw 'this build stamps a version resource: pass -Version' }; return $Map.version }
                default { if (-not $Map.ContainsKey($kind)) { throw "unknown placeholder {$kind}" }; return [string]$Map[$kind] }
            }
        })
    if ($isPath) { $value = $value -replace '/', '\' }
    return $value
}

function Get-Digest($Context, [string]$File) {
    $line = & python $Context.Provenance digest $File
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($line -split '\s+')[1]
}

# Removes every tree that New-Tree made through this context: a work tree through its checkout, a clone as a plain
# directory. A work tree that git does not remove is deleted and pruned.
function Remove-Trees($Context) {
    foreach ($t in $Context.Trees) {
        if (-not $t.checkout) {
            Remove-Item -LiteralPath $t.tree -Recurse -Force -ErrorAction SilentlyContinue
            continue
        }
        Invoke-Locked $Context {
            & git -C $t.checkout worktree remove --force $t.tree 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0 -and (Test-Path -LiteralPath $t.tree)) {
                Remove-Item -LiteralPath $t.tree -Recurse -Force -ErrorAction SilentlyContinue
                & git -C $t.checkout worktree prune
            }
        }
    }
    $Context.Trees.Clear()
}

Export-ModuleMember -Function New-BuildContext, Invoke-Locked, Get-Checkout, New-Tree, Initialize-Submodules,
    Export-Input, Resolve-Inputs, Resolve-ToolPath, Expand-Arg, Get-Digest, Remove-Trees
