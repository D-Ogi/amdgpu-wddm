#Requires -Version 7.0
# Rebuilds release payload files from the commits that release-sources.json names, in temporary git work trees,
# and compares the bytes with the recorded ones.
#
#   pwsh -File tools\release\rebuild-check.ps1 [-Sources <release-sources.json>] [-Root <BC250_ROOT>]
#        [-Path <payload path>[,...]] [-IncludeUnverified] [-Twice] [-Plan] [-Keep] [-Work <dir>]
#        [-Tool widl=<path>,ninja=<path>] [-PackageCache <dir>] [-Report <file.json>]
#
# What it rebuilds: every files[] entry whose built_from names a recipe. By default only the entries that claim a
# bit-identical rebuild (no "unverified" field); -IncludeUnverified adds the others, -Path picks entries by their
# package path. Entries that one build makes together (the same repository, commit, recipe, recipe commit, arguments
# and inputs) share one build.
#
# For each build, under -Work (default <BC250_ROOT>\scratch\repro-builds\rb: short, for the deep paths of Mesa), in <recipe>-<hash>\a:
#   src      a detached work tree of the build repository at the commit (git worktree add), with its submodules
#            cloned from the checkout's own module repositories (no network);
#   recipe   a work tree of the recipe repository at recipe_commit, when that is not src;
#   in\<n>   each pinned input: the listed paths of its commit (git archive), a work tree of its commit with the
#            submodules when it lists no paths, or an inline JSON object as a file;
#   out      the empty output directory;
#   tmp      the build's own temporary directory ({tmp}: a recipe's -Temp, so that builds share no TEMP).
# The trees, inputs and placeholders are the functions of BuildTree.psm1, which build-release-set.ps1 uses as well.
# Argument placeholders: {src} {out} {tmp} {root} {input:<name>} {payload:<package path>} (the source file of that entry),
# {tool:<name>} (-Tool, then the environment variable BC250_TOOL_<NAME>, then PATH), and
# {version} {flavour}: the release this package is, which a build that stamps the version resource of a
# shipped file takes as an argument (BD-166). -Version and -Flavour give them; without -Version the
# rebuild of such an entry would differ from the pinned file, so an entry that uses the placeholder and
# was given no version is refused.
# Outputs are compared by SHA-256; a signed file also by "unsigned_sha256" against the recipe's "unsigned_output" and
# by its Authenticode digest. MESON_PACKAGE_CACHE_DIR points at -PackageCache, so a Meson wrap (Mesa's zlib) needs
# no download. The work trees are removed at the end unless -Keep. The directory of a build with an entry that does
# not match keeps its build-*.log files (a setup failure writes its message there), so the path in the FAIL line can
# still be read; -Keep keeps the trees and outputs too.
#
# -Twice builds each group a second time from new work trees in <recipe>-<hash>\bb (other paths, another path
# length) and compares the two builds too: this is the determinism measurement of docs/design/reproducible-builds.md.
# -Plan prints the builds and their expanded arguments and builds nothing.
# Exit 0 when every entry that claims a bit-identical rebuild matches (or, for a signed file, matches as an image),
# and with -Twice also when the two builds agree; 1 otherwise. An unverified entry that matches is reported, so its
# "unverified" field can go.
[CmdletBinding()]
param(
    [string]$Sources = (Join-Path $PSScriptRoot 'release-sources.json'),
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [string[]]$Path,
    [switch]$IncludeUnverified,
    [switch]$Twice,
    [switch]$Plan,
    [switch]$Keep,
    [string]$Work,
    [string[]]$Tool,
    [string]$PackageCache,
    [string]$Report,
    # The release version and flavour for the {version} and {flavour} placeholders (BD-166).
    [string]$Version = '',
    [ValidateSet('release', 'lab')][string]$Flavour = 'release'
)
# Release and provenance rebuilds always execute the gates, including nested analysis.
$env:BC250_GATE_NOCACHE = '1'
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Work) { $Work = Join-Path $Root 'scratch\repro-builds\rb' }
if (-not $PackageCache) { $PackageCache = Join-Path $Root 'scratch\repro-builds\packagecache' }
$Path = @($Path | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$tools = @{}
foreach ($t in @($Tool | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
    $k, $v = $t -split '=', 2
    if (-not $v) { throw "-Tool takes name=path, not '$t'" }
    $tools[$k.ToLowerInvariant()] = $v
}
$manifest = Get-Content -LiteralPath $Sources -Raw | ConvertFrom-Json -AsHashtable
$recipeRepo = $manifest.recipe_repository
$sourceOf = @{}
foreach ($f in $manifest.files) { $sourceOf[$f.path] = Join-Path $Root ($f.source -replace '/', '\') }

# The work trees, the inputs and the argument placeholders: BuildTree.psm1, which build-release-set.ps1 shares.
Import-Module (Join-Path $PSScriptRoot 'BuildTree.psm1') -Force
$ctx = New-BuildContext -Sources $Sources -Root $Root -Repo $repo -Tools $tools -SourceOf $sourceOf
$script:dirs = [Collections.Generic.List[string]]::new()
$script:keepLogs = [Collections.Generic.HashSet[string]]::new()

# The entries to rebuild, grouped by build.
$selected = @(foreach ($f in $manifest.files) {
        $b = $f.built_from
        if (-not $b -or -not $b.recipe) { continue }
        if ($Path.Count) { if ($f.path -notin $Path) { continue } }
        elseif ($f.unverified -and -not $IncludeUnverified) { continue }
        $f
    })
if ($Path.Count) {
    foreach ($p in $Path) { if ($p -notin @($selected | ForEach-Object { $_.path })) { throw "-Path ${p}: no entry with a built_from recipe" } }
}
$groups = [ordered]@{}
foreach ($f in $selected) {
    $b = $f.built_from
    $key = [ordered]@{ repo = $b.repo; commit = $b.commit; recipe = $b.recipe
        recipe_commit = $(if ($b.recipe_commit) { $b.recipe_commit } else { $b.commit }); args = @($b.args); inputs = $b.inputs } | ConvertTo-Json -Depth 10 -Compress
    if (-not $groups.Contains($key)) { $groups[$key] = [Collections.Generic.List[object]]::new() }
    $groups[$key].Add($f)
}
"rebuild-check: $($selected.Count) entries in $($groups.Count) builds$(if ($Twice) { ', each built twice' })"

$results = [Collections.Generic.List[object]]::new()
$savedCache = $env:MESON_PACKAGE_CACHE_DIR
$savedRoot = $env:BC250_ROOT
$savedCompat = $env:__COMPAT_LAYER
try {
    $env:BC250_ROOT = $Root
    # An old recipe runs its 32-bit host tests without this. Windows installer detection then asks for elevation for
    # hosted-dispatch-test.exe (the name contains "patch"), and the build waits for a UAC prompt on the desktop.
    $env:__COMPAT_LAYER = 'RunAsInvoker'
    if (Test-Path -LiteralPath $PackageCache) { $env:MESON_PACKAGE_CACHE_DIR = $PackageCache }
    $index = 0
    foreach ($key in $groups.Keys) {
        $index++
        $entries = $groups[$key]
        $b = $entries[0].built_from
        $recipeCommit = if ($b.recipe_commit) { $b.recipe_commit } else { $b.commit }
        $sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($key))).Replace('-', '').Substring(0, 8)
        $dir = Join-Path $Work ('{0}-{1}' -f [IO.Path]::GetFileNameWithoutExtension($b.recipe), $sha)
        $label = '{0} {1} {2} {3}' -f $b.repo, $b.commit.Substring(0, 8), $b.recipe, (@($b.args) -join ' ')
        "[$index/$($groups.Count)] $label"
        $runs = @(@{ tag = 'first'; base = Join-Path $dir 'a' })
        if ($Twice) { $runs += @{ tag = 'second'; base = Join-Path $dir 'bb' } }
        foreach ($r in $runs) {
            $r.map = @{ src = Join-Path $r.base 'src'; out = Join-Path $r.base 'out'; tmp = Join-Path $r.base 'tmp'; root = $Root
                inputs = (Resolve-Inputs $ctx $b.inputs (Join-Path $r.base 'in') -NoCreate); version = $Version; flavour = $Flavour }
            $r.recipeTree = if ($b.repo -eq $recipeRepo -and $recipeCommit -eq $b.commit) { $r.map.src } else { Join-Path $r.base 'recipe' }
        }
        if ($Plan) {
            foreach ($r in $runs) {
                $expanded = @(foreach ($a in @($b.args)) { try { Expand-Arg $ctx $a $r.map } catch { "<$($_.Exception.Message)>" } })
                "  $($r.tag): $($r.recipeTree)\$($b.recipe -replace '/', '\') $($expanded -join ' ')"
            }
            foreach ($f in $entries) { "    -> $($f.path) = out\$($f.built_from.output -replace '/', '\')$(if ($f.unverified) { '  (unverified)' })" }
            continue
        }
        if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
        New-Item -ItemType Directory -Force $dir | Out-Null
        $script:dirs.Add($dir)
        $built = @{}
        foreach ($r in $runs) {
            $log = Join-Path $dir "build-$($r.tag).log"
            try {
                New-Tree $ctx $b.repo $b.commit $r.map.src
                if ($r.recipeTree -ne $r.map.src) { New-Tree $ctx $recipeRepo $recipeCommit $r.recipeTree }
                $null = Resolve-Inputs $ctx $b.inputs (Join-Path $r.base 'in')
                New-Item -ItemType Directory -Force $r.map.out, $r.map.tmp | Out-Null
                $expanded = @(foreach ($a in @($b.args)) { Expand-Arg $ctx $a $r.map })
                $recipeFile = Join-Path $r.recipeTree ($b.recipe -replace '/', '\')
                $watch = [Diagnostics.Stopwatch]::StartNew()
                if ($recipeFile.EndsWith('.py')) { & python $recipeFile @expanded *> $log } else { & pwsh -NoProfile -File $recipeFile @expanded *> $log }
                $code = $LASTEXITCODE
                '  {0} build: exit {1} after {2} s ({3})' -f $r.tag, $code, [int]$watch.Elapsed.TotalSeconds, $log
                $built[$r.tag] = @{ code = $code; out = $r.map.out; log = $log }
            } catch {
                "  $($r.tag) build setup failed: $($_.Exception.Message)"
                Add-Content -LiteralPath $log -Value "build setup failed: $($_.Exception.Message)"
                $built[$r.tag] = @{ code = -1; out = $null; log = $log }
            }
        }
        foreach ($f in $entries) {
            $res = [ordered]@{ path = $f.path; build = $label; claimed = (-not $f.unverified); status = $null; sha256 = $null; detail = @() }
            $first = $built['first']
            if (-not $first -or $first.code -ne 0) {
                $res.status = 'build-failed'; $res.detail += "see $($first.log)"
            } else {
                $file = Join-Path $first.out ($f.built_from.output -replace '/', '\')
                if (-not (Test-Path -LiteralPath $file)) {
                    $res.status = 'missing-output'; $res.detail += $file
                } else {
                    $res.sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
                    if ($res.sha256 -eq $f.sha256) { $res.status = 'match' }
                    else {
                        $res.status = 'differs'
                        if ($f.unsigned_sha256 -and $f.built_from.unsigned_output) {
                            $u = Join-Path $first.out ($f.built_from.unsigned_output -replace '/', '\')
                            if ((Test-Path -LiteralPath $u) -and (Get-FileHash -LiteralPath $u -Algorithm SHA256).Hash -eq $f.unsigned_sha256) {
                                $res.status = 'match-image'; $res.detail += 'the unsigned image matches; the signature differs'
                            }
                        }
                        if ($res.status -eq 'differs' -and $f.authenticode_sha256 -and (Get-Digest $ctx $file) -eq $f.authenticode_sha256) {
                            $res.status = 'match-image'; $res.detail += 'the Authenticode digest matches; the signature differs'
                        }
                    }
                    if ($Twice) {
                        $second = $built['second']
                        $file2 = if ($second -and $second.code -eq 0) { Join-Path $second.out ($f.built_from.output -replace '/', '\') } else { $null }
                        $res['second_sha256'] = if ($file2 -and (Test-Path -LiteralPath $file2)) { (Get-FileHash -LiteralPath $file2 -Algorithm SHA256).Hash } else { $null }
                        $res['deterministic'] = ($res.second_sha256 -eq $res.sha256)
                    }
                }
            }
            if ($res.status -notin 'match', 'match-image' -or ($Twice -and -not $res.deterministic)) { [void]$script:keepLogs.Add($dir) }
            $results.Add([pscustomobject]$res)
        }
    }
} finally {
    $env:MESON_PACKAGE_CACHE_DIR = $savedCache
    $env:BC250_ROOT = $savedRoot
    $env:__COMPAT_LAYER = $savedCompat
    if (-not $Keep) {
        Remove-Trees $ctx
        foreach ($d in $script:dirs) {
            if ($script:keepLogs.Contains($d)) {
                Get-ChildItem -LiteralPath $d -Force | Where-Object { $_.Name -notlike 'build-*.log' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
            } else { Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
}
if ($Plan) { return }

$failed = 0
foreach ($r in $results) {
    $ok = $r.status -in 'match', 'match-image'
    if ($Twice -and -not $r.deterministic) { $ok = $false }
    $tag = if ($ok) { 'OK  ' } elseif ($r.claimed) { 'FAIL' } else { 'note' }
    if (-not $ok -and $r.claimed) { $failed++ }
    $sha = if ($r.sha256) { $r.sha256.Substring(0, 8) } else { '-' }
    $two = if ($Twice) { " second $(if ($r.second_sha256) { $r.second_sha256.Substring(0, 8) } else { '-' }) $(if ($r.deterministic) { 'deterministic' } else { 'NOT deterministic' })" } else { '' }
    '{0} {1,-13} {2}  {3}{4}{5}' -f $tag, $r.status, $sha, $r.path, $two, $(if ($r.detail) { "  ($($r.detail -join '; '))" } else { '' })
    if (-not $r.claimed -and $r.status -eq 'match') { "     $($r.path) is marked unverified, but its rebuild matches: drop its 'unverified' field" }
}
if ($Report) { $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Report -Encoding utf8 }
"rebuild-check: $(@($results | Where-Object { $_.status -in 'match', 'match-image' }).Count) of $($results.Count) match; $failed claimed entries do not"
exit $(if ($failed) { 1 } else { 0 })
