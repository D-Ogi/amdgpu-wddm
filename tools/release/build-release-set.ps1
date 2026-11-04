#Requires -Version 7.0
# The one-command build of a release set: every component build that a release manifest names, from the commits it
# pins, after the gates, in parallel within the disk budget of this workspace, with the host tests of each line, and
# the release-sources.json rows of what it built.
#
#   pwsh -File tools\release\build-release-set.ps1 -Manifest <release.json> -Out <dir under BC250_ROOT>
#        [-Root <BC250_ROOT>] [-Sources <release-sources.json>] [-Only <id or pattern>[,...]] [-Plan]
#        [-AllowUnpushed] [-MaxParallel <n>] [-Jobs <n>] [-Version <v>] [-Work <dir>] [-Tool name=path[,...]]
#        [-PackageCache <dir>] [-SizeGuard <size-guard.py>] [-GuardMaxAge <s>] [-BuildTimeout <s>] [-KeepGoing]
#        [-Keep] [-Compare <release-sources.json>] [-MergeInto <release-sources.json>] [-Register]
#
# The manifest (release-manifest.schema.json; release-tester.32.json is an example) has one row per component build:
# the repository and commit, the recipe of this repository and its arguments, the flavour, the architecture, the
# compiler, and the files the build makes with the package paths they ship as. build_release_set.py reads it and plans.
#
# 1. Gates, before any build (build_release_set.py gates, then the size guard):
#    commits      every commit (build, recipe, pinned input, mapped submodule) is in a checkout;
#    published    a remote-tracking branch of the repository's URL holds it: the D-Ogi fork of each line. A commit
#                 that is not pushed is refused, unless -AllowUnpushed (a local dry run: a warning in the report);
#    attribution  no commit from the base (the commit the previous release pinned) to the pinned commit names an AI
#                 agent or model as author, committer, co-author or in its message;
#    compiler     each row names its compiler, the recipe's command line names it too where the recipe takes
#                 -Compiler, and the compiler policy allows it (clang-cl for user mode, msvc with a reason);
#    state-history  for each row that builds the state worker (driver/state/installer-build.ps1), the state catalog
#                 of every published tester tag with a version below -Version is in driver/state/history at
#                 the row's commit, and each history file is named by its catalog identity
#                 (tools/state/check_history.py): else the worker moves the removal record of that release aside;
#    size         the size guard (scratch\cleanup-250\size-guard.py) says the workspace is under its warning lines.
#    host         a warning only: the compiler processes of other builds that run on this PC at the start.
# 2. Builds, in parallel: at most -MaxParallel slots at once (a row may take several), each build with
#    {jobs} = slots x (logical processors / -MaxParallel), unless -Jobs gives the jobs of one slot. A build starts only
#    when its peak disk use (disk_peak_gb of the row) still fits under the size guard's warning lines, counted from the
#    guard's last measurement plus the reservations of the builds that run (BuildTree.psm1 makes the trees: a work
#    tree of the commit with its submodules from the checkout, a clone, or one read-only work tree that every row of
#    one repository and commit shares, which must still be clean when the build ends). Each build runs in its own
#    process (this script with -Job), under <Work>\<row id> (short, for the deep paths of Mesa), with its own TEMP.
#    When it ends, its outputs and their PDBs go to <Out>\artifacts\<flavour>\<row id>\ (build_release_set.py collect) and
#    its tree goes at once; its logs stay in <Out>\logs\<row id>\. A failed build fails the release set; no new build starts
#    after it unless -KeepGoing, and a build that needs its outputs does not start at all.
# 3. Host tests: the 'tests' of a row run on the build's own output before the trim, and the numbers of each test's
#    summary lines go into the report.
# 4. Rows: <Out>\release-sources.rows.json holds a release-sources.json row for every output (SHA-256, size, source,
#    built_from with the recipe commit, arguments and inputs) and, beside them, the PDB of each image. -Compare holds
#    them against a registered release-sources.json (the same bytes where the row claims a deterministic build, or the
#    same image without its signature); -MergeInto writes <Out>\release-sources.json, a copy of that file with these
#    rows in place of the old ones, for build-release.ps1 (with -BuildRecords <Out>\artifacts: each build's
#    recipe.json is kept beside its outputs). <Out>\release-report.json holds every gate, build, test and time.
#    -Register writes the rows into tools\release\release-sources.json of this tree and rebinds each D3D11 deployment
#    record (amdgpu_wddm_d3d11.config) whose engine or ICD changed to their new SHA-256 (<Out>\configs, the receipt
#    <Out>\register-receipt.json). The capability words of a record are not measured again. Commit and push the file.
# 5. Packages, a second run on the registered set:
#      pwsh -File tools\release\build-release-set.ps1 -Package -From <Out> -UmdSource <dir> -UmdBuild <dir>
#           [-Flavours release,lab] [-LedgerDebtReason <text>] [-Parallel] [-SetAside <round name>]
#    Before the first step, it names in one message every output of an earlier round that a step would refuse
#    (setup, package, submission CAB, package worktrees; PackageRound.psm1). -SetAside <name> moves them into
#    <Out>\rounds\<name>\ instead, with the SHA-256 of each file before and after in set-aside.json (BD-272).
# 6. A notes-only repack, when the notes change after the packages are built and gated:
#      pwsh -File tools\release\build-release-set.ps1 -RepackNotes -From <Out> [-Flavours release,lab]
#    For each flavour, repack-notes.ps1 makes <Out>\package-notes\<flavour> from <Out>\package\<flavour> with the
#    committed notes of this tree: every file but the notes and manifest.json byte for byte, manifest.json changed in
#    the notes values only, the zip and the single exe made again. Then the flavour gate and the installer dry run on
#    the new package. <Out>\repack-report.json holds the new zip and exe hashes (BD-265). The repack signs the new
#    single exe and, for the release flavour, writes and signs nagi-update.json over the new zip (BD-294).
#    The tree must be clean and pushed, and its release-sources.json must hold every row of <Out> unchanged. For each
#    flavour it builds the setup window, sets the router inputs of the quality gates and runs build-release.ps1 with
#    -BuildRecords <Out>\artifacts (the release flavour also writes the submission CAB). <Out>\package\<flavour> holds
#    each package and its zip, and <Out>\package-report.json their SHA-256. Then the host dry run of the installer
#    (test-dryrun.ps1) on each package and the BD-167 byte check of the Mesa DLLs of both (flavour_bytes.py).
#    -Parallel builds the flavours at the same time. Each flavour runs in its own process (the package steps set
#    process-wide variables such as TEMP) and in its own detached worktree of the same commit, <Out>\package-wt-<flavour>
#    (the setup window build writes intermediate files into the tree). Each process writes
#    <Out>\package-report-<flavour>.json and <Out>\logs\package-<flavour>.out; this process merges the reports, runs
#    the byte check and removes the worktrees.
#
# -Plan prints the gates, the builds with their expanded command lines, outputs, tests and disk budget, and builds
# nothing. -Only rebuilds the rows it names (ids or wildcards); a row it does not name keeps the artifact of the last
# run in -Out, when that run built it from the same manifest row, and the rows cover both. A row whose manifest entry
# differs only in its recipe commit keeps its artifact when the recipe's directory is the same at both commits (git
# diff); its rows keep the commit that built it. A run that ends before a build starts (a failed gate, a build that
# did not start) keeps the earlier builds of every row in release-report.json, so the next run still finds them.
# -KeepFrom <earlier -Out> imports the ok builds of that directory for the rows that -Only does not name and -Out has
# no build of: the same rule, a SHA-256 check of every file at the source, a copy into <Out>\artifacts and the same
# check on the copy, before any build starts (BD-244). -KeepFrom without -Only builds only what changed: each row
# that an earlier build in -Out or -KeepFrom stands for (the rule above) is kept or imported, and every other row is
# built, together with each row that takes a payload of a row that is built. Give the set directory of the release
# before (for example scratch\t37\release\set): a release that moves two rows then builds two rows, not all of them.
# Exit 0 when every gate passes and every build ends well (with -Compare: and every output that claims a deterministic
# build matches); 1 otherwise.
[CmdletBinding(DefaultParameterSetName = 'Set')]
param(
    [Parameter(ParameterSetName = 'Set', Mandatory)][string]$Manifest,
    [Parameter(ParameterSetName = 'Set', Mandatory)][string]$Out,
    [Parameter(ParameterSetName = 'Set')]
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    # A release-sources.json for the {payload:} inputs that no row of this release set builds.
    [Parameter(ParameterSetName = 'Set')][string]$Sources,
    [Parameter(ParameterSetName = 'Set')][string[]]$Only,
    [Parameter(ParameterSetName = 'Set')][switch]$Plan,
    [Parameter(ParameterSetName = 'Set')][switch]$AllowUnpushed,
    [Parameter(ParameterSetName = 'Set')][ValidateRange(1, 64)][int]$MaxParallel = 3,
    [Parameter(ParameterSetName = 'Set')][int]$Jobs = 0,
    # The release version for {version}; the manifest's own by default.
    [Parameter(ParameterSetName = 'Set')][string]$Version = '',
    [Parameter(ParameterSetName = 'Set')][string]$Work,
    [Parameter(ParameterSetName = 'Set')][string[]]$Tool,
    [Parameter(ParameterSetName = 'Set')][string]$PackageCache,
    [Parameter(ParameterSetName = 'Set')][string]$SizeGuard,
    # A size guard record younger than this many seconds is used as it is; an older one is measured again.
    [Parameter(ParameterSetName = 'Set')][int]$GuardMaxAge = 600,
    [Parameter(ParameterSetName = 'Set')][int]$BuildTimeout = 7200,
    [Parameter(ParameterSetName = 'Set')][switch]$KeepGoing,
    [Parameter(ParameterSetName = 'Set')][switch]$Keep,
    # With -Only: an earlier -Out (another round) whose ok builds may stand for the rows that -Only does not name, when
    # -Out has no build of them. They are copied into -Out after a SHA-256 check (see "-KeepFrom" below).
    [Parameter(ParameterSetName = 'Set')][string]$KeepFrom,
    [Parameter(ParameterSetName = 'Set')][string]$Compare,
    [Parameter(ParameterSetName = 'Set')][string]$MergeInto,
    # After the rows: write them into this tree's tools\release\release-sources.json and rebind the D3D11 records.
    [Parameter(ParameterSetName = 'Set')][switch]$Register,
    # One build of a release set, in its own process: the orchestrator below starts this script with the job's file.
    [Parameter(ParameterSetName = 'Job', Mandatory)][string]$Job,
    # The packages of a built and registered release set (see "5. Packages" above).
    [Parameter(ParameterSetName = 'Package', Mandatory)][switch]$Package,
    [Parameter(ParameterSetName = 'Package', Mandatory)][Parameter(ParameterSetName = 'Repack', Mandatory)][string]$From,
    [Parameter(ParameterSetName = 'Package')][Parameter(ParameterSetName = 'Repack')][string[]]$Flavours = @('release', 'lab'),
    # A notes-only repack of the packages of -From (BD-265, repack-notes.ps1): the notes of this tree's
    # docs\testing\release-notes, every other file byte for byte, then the flavour gate and the installer dry run.
    [Parameter(ParameterSetName = 'Repack', Mandatory)][switch]$RepackNotes,
    # The user-mode driver tree that the release gates of build-release.ps1 read (BC250_UMD_SOURCE, BC250_UMD_BUILD).
    [Parameter(ParameterSetName = 'Package', Mandatory)][string]$UmdSource,
    [Parameter(ParameterSetName = 'Package', Mandatory)][string]$UmdBuild,
    # The folder of the PFX, its password file and the test certificate that -RepackNotes signs with (BD-294);
    # default <PackageRoot>\secrets\release, as build-release.ps1 reads them. A host test gives a folder of its own.
    [Parameter(ParameterSetName = 'Repack')][string]$SigningSecrets,
    # Unlanded ledger work this release carries on purpose (build-release.ps1 -AllowLedgerDebt).
    [Parameter(ParameterSetName = 'Package')][string]$LedgerDebtReason = '',
    [Parameter(ParameterSetName = 'Package')][Parameter(ParameterSetName = 'Repack')][string]$PackageRoot = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path (Split-Path (Split-Path $PSScriptRoot)) }),
    [Parameter(ParameterSetName = 'Package')][string]$PackageGuard,
    # Build the flavours at the same time, each in its own process and worktree (see "5. Packages" above).
    [Parameter(ParameterSetName = 'Package')][switch]$Parallel,
    # Move the outputs of an earlier package round in -From into <From>\rounds\<name>\ (SHA-256 checked) instead of
    # refusing them (BD-272, PackageRound.psm1).
    [Parameter(ParameterSetName = 'Package')][string]$SetAside,
    # The package report; <From>\package-report.json by default. A -Parallel run gives each flavour its own. Not named
    # -Report: PowerShell variable names ignore case, and the release set path uses a $report table.
    [Parameter(ParameterSetName = 'Package')][string]$FlavourReport
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
$buildReleaseSet = Join-Path $PSScriptRoot 'build_release_set.py'
$shell = (Get-Process -Id $PID).Path
Import-Module (Join-Path $PSScriptRoot 'BuildTree.psm1') -Force

function Get-Utc { [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') }

function Write-Json([string]$Path, $Data) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    [IO.File]::WriteAllText($Path, ($Data | ConvertTo-Json -Depth 40) + "`n")
}

function Read-Json([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable }

# The bytes of every file under a directory, links not followed. A file that goes during the walk counts as gone.
function Get-TreeBytes([string]$Dir) {
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir)) { return [long]0 }
    $options = [IO.EnumerationOptions]::new()
    $options.RecurseSubdirectories = $true
    $options.IgnoreInaccessible = $true
    $options.AttributesToSkip = [IO.FileAttributes]::ReparsePoint
    $sum = [long]0
    try {
        foreach ($f in [IO.DirectoryInfo]::new($Dir).EnumerateFiles('*', $options)) { try { $sum += $f.Length } catch { } }
    } catch { }
    return $sum
}

# The placeholder map of one build (BuildTree.psm1 Expand-Arg): its directories, the version and the jobs.
function New-ArgMap($Build, [string]$SetRoot, [string]$ReleaseVersion, $InputMap) {
    return @{ src = $Build.dirs.src; out = $Build.dirs.out; tmp = $Build.dirs.tmp; root = $SetRoot
        inputs = $InputMap; version = $(if ($Build.version) { $Build.version } else { $ReleaseVersion })
        flavour = $Build.flavour; jobs = $Build.jobs; recipe = $Build.dirs.recipe; testout = $Build.dirs.tests }
}

# A test of a row (the 'tests' of the manifest), on the build's own output. Returns the report entry.
function Invoke-HostTest($Context, $Test, [hashtable]$Map, [string]$RecipeTree, [string]$LogDir, [string]$Python) {
    $entry = [ordered]@{ name = $Test.name; status = $null; exit_code = $null; seconds = 0; log = $null; summary = [ordered]@{}; cases = 0; reason = $null }
    if ($Test.requires) {
        $need = Expand-Arg $Context $Test.requires $Map
        if (-not [IO.Path]::IsPathRooted($need)) { $need = Join-Path $Map.src $need }
        if (-not (Test-Path -LiteralPath $need)) {
            $entry.status = 'skipped'; $entry.reason = "this tree has no $need"
            return $entry
        }
    }
    $run = Expand-Arg $Context $Test.run $Map
    if (-not [IO.Path]::IsPathRooted($run)) { $run = Join-Path $RecipeTree ($run -replace '/', '\') }
    $testArgs = @(foreach ($a in @($Test.args)) { if ($null -ne $a) { Expand-Arg $Context $a $Map } })
    $entry.log = Join-Path $LogDir "test-$($Test.name).log"
    $saved = @{}
    foreach ($k in @(($Test.env ?? @{}).Keys)) {
        $saved[$k] = [Environment]::GetEnvironmentVariable($k)
        $value = Expand-Arg $Context $Test.env[$k] $Map
        # A variable that names a place of the test's own output gets that directory: a program such as the DXVK
        # engine writes its log only into a directory that exists, and its test then failed on an empty log.
        if ($Test.env[$k] -like '{testout}*' -and $value) { New-Item -ItemType Directory -Force $value | Out-Null }
        [Environment]::SetEnvironmentVariable($k, $value)
    }
    $cwd = if ($Test.cwd) { Expand-Arg $Context $Test.cwd $Map } else { $Map.out }
    New-Item -ItemType Directory -Force $cwd | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Push-Location -LiteralPath $cwd
    try {
        if ($run.EndsWith('.ps1')) { & $shell -NoProfile -File $run @testArgs *> $entry.log }
        elseif ($run.EndsWith('.py')) { & $Python $run @testArgs *> $entry.log }
        else { & $run @testArgs *> $entry.log }
        $entry.exit_code = $LASTEXITCODE
    } finally {
        Pop-Location
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    $entry.seconds = [int]$watch.Elapsed.TotalSeconds
    $lines = @(Get-Content -LiteralPath $entry.log -ErrorAction SilentlyContinue)
    if ($Test.summary) {
        $rx = [regex]::new($Test.summary)
        foreach ($line in $lines) {
            $m = $rx.Match($line)
            if (-not $m.Success) { continue }
            $entry.cases++
            foreach ($g in $m.Groups) {
                if ($g.Name -match '^\d+$' -or -not $g.Success) { continue }
                $n = 0L
                if ([long]::TryParse($g.Value, [ref]$n)) { $entry.summary[$g.Name] = ($entry.summary[$g.Name] ?? 0) + $n }
            }
        }
    }
    $passLine = if ($Test.pass) { @($lines | Where-Object { $_ -match $Test.pass }).Count -gt 0 } else { $true }
    $failures = if ($entry.summary.Contains('failures')) { $entry.summary.failures } else { 0 }
    $entry.status = if ($entry.exit_code -eq 0 -and $passLine -and $failures -eq 0) { 'pass' } else { 'fail' }
    if ($entry.status -eq 'fail') {
        $entry.reason = if ($entry.exit_code -ne 0) { "exit $($entry.exit_code)" } elseif (-not $passLine) { "no line matches $($Test.pass)" } else { "$failures failure(s)" }
    }
    return $entry
}

# ---------------------------------------------------------------------------------------------------------------
# One build, in its own process.
function Invoke-SetJob([string]$SpecPath) {
    $spec = Read-Json $SpecPath
    $b = $spec.build
    $d = $b.dirs
    $logDir = $spec.log_dir
    $result = [ordered]@{ id = $b.id; status = 'failed'; error = $null; exit_code = $null; started_utc = (Get-Utc)
        seconds = 0; build_seconds = 0; tests = @(); outputs = @(); records = @(); shared_clean = $null; command = $null }
    $ctx = New-BuildContext -Sources $spec.manifest -Root $spec.root -Repo $spec.repo -Tools $spec.tools -SourceOf $spec.payload -Lock $spec.lock
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        if (Test-Path -LiteralPath $d.work) { Remove-Item -LiteralPath $d.work -Recurse -Force }
        New-Item -ItemType Directory -Force $d.out, $d.tmp, $d.tests | Out-Null
        $env:BC250_ROOT = $spec.root
        # An old recipe runs its 32-bit host tests without this; Windows installer detection then asks for elevation
        # for an executable whose name contains "patch", and the build waits for a UAC prompt (rebuild-check.ps1).
        $env:__COMPAT_LAYER = 'RunAsInvoker'
        $env:BC250_GATE_NOCACHE = '1'
        $env:PYTHONDONTWRITEBYTECODE = '1'
        $env:TEMP = $d.tmp; $env:TMP = $d.tmp
        # Each build gets its own copy of the shared Meson package cache. Two Meson runs that download the same
        # wrap into one cache at the same time race on the final rename, which Windows refuses when the file is
        # already there (zlib, two Mesa lines at once). A file this build fetched goes back into the shared cache.
        $ownCache = $null
        if ($spec.package_cache -and (Test-Path -LiteralPath $spec.package_cache)) {
            $ownCache = Join-Path $d.work 'pc'
            New-Item -ItemType Directory -Force $ownCache | Out-Null
            Get-ChildItem -LiteralPath $spec.package_cache -File | Where-Object { $_.Name -notlike 'tmp*' } |
                ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $ownCache }
            $env:MESON_PACKAGE_CACHE_DIR = $ownCache
        }
        if (-not $d.src_shared) { New-Tree $ctx $b.repo $b.commit $d.src -Clone:($b.tree -eq 'clone') }
        if ($d.recipe -ne $d.src -and -not $d.recipe_shared) { New-Tree $ctx $spec.recipe_repository $b.recipe_commit $d.recipe }
        $inputMap = Resolve-Inputs $ctx $b.inputs $d.inputs
        $map = New-ArgMap $b $spec.root $spec.version $inputMap
        $expanded = @(foreach ($a in @($b.args)) { Expand-Arg $ctx $a $map })
        $recipeFile = Join-Path $d.recipe ($b.recipe -replace '/', '\')
        $result.command = "$recipeFile $($expanded -join ' ')"
        "$(Get-Utc) build $($b.id): $($result.command)"
        $log = Join-Path $logDir 'build.log'
        $buildWatch = [Diagnostics.Stopwatch]::StartNew()
        if ($recipeFile.EndsWith('.py')) { & $spec.python $recipeFile @expanded *> $log } else { & $shell -NoProfile -File $recipeFile @expanded *> $log }
        $result.exit_code = $LASTEXITCODE
        $result.build_seconds = [int]$buildWatch.Elapsed.TotalSeconds
        "$(Get-Utc) recipe exit $($result.exit_code) after $($result.build_seconds) s"
        if ($result.exit_code -ne 0) { throw "the recipe exited with $($result.exit_code) (see $log)" }
        $tests = @()
        foreach ($t in @($b.tests)) {
            if ($null -eq $t) { continue }
            $entry = Invoke-HostTest $ctx $t $map $d.recipe $logDir $spec.python
            "$(Get-Utc) test $($t.name): $($entry.status)$(if ($entry.cases) { ", $($entry.cases) summary lines" })$(if ($entry.summary.Count) { ', ' + (($entry.summary.Keys | ForEach-Object { "$_=$($entry.summary[$_])" }) -join ' ') })$(if ($entry.reason) { " ($($entry.reason))" })"
            $tests += $entry
        }
        $result.tests = $tests
        $failedTests = @($tests | Where-Object { $_.status -eq 'fail' })
        if ($failedTests.Count) { throw "host test(s) $(($failedTests | ForEach-Object { $_.name }) -join ', ') failed (see $logDir)" }
        # A tree that several builds read must come out of each of them as it went in.
        $dirty = @()
        foreach ($tree in @(@($d.src, $d.src_shared), @($d.recipe, $d.recipe_shared))) {
            if (-not $tree[1]) { continue }
            $status = @(& git --no-optional-locks -C $tree[0] status --porcelain)
            if ($status.Count) { $dirty += "$($tree[0]): $($status[0])" }
        }
        $result.shared_clean = -not $dirty.Count
        if ($dirty.Count) { throw "the build changed a shared tree: $($dirty -join '; ')" }
        $collectSpec = Join-Path $logDir 'collect-job.json'
        Write-Json $collectSpec ([ordered]@{ out = $d.out; artifact = $b.artifact; outputs = $b.outputs; flavour = $b.flavour
                keep = @($b.keep | Where-Object { $_ }); log_dir = $logDir })
        & $spec.python $spec.build_release_set collect --job $collectSpec *> (Join-Path $logDir 'collect.log')
        $code = $LASTEXITCODE
        $collected = Read-Json (Join-Path $logDir 'collect.json')
        $result.outputs = $collected.outputs
        $result.records = $collected.records
        Get-Content -LiteralPath (Join-Path $logDir 'collect.log')
        if ($code -ne 0) { throw "collect: $(@($collected.errors) -join '; ')" }
        $result.status = 'ok'
    } catch {
        $result.error = $_.Exception.Message
        "$(Get-Utc) FAIL $($result.error)"
    } finally {
        if ($ownCache -and (Test-Path -LiteralPath $ownCache)) {
            foreach ($f in Get-ChildItem -LiteralPath $ownCache -File | Where-Object { $_.Name -notlike 'tmp*' }) {
                $dest = Join-Path $spec.package_cache $f.Name
                if (Test-Path -LiteralPath $dest) { continue }
                # A copy under a name of its own, then a rename that fails harmlessly if another build was first.
                $part = "$dest.$PID.part"
                try { Copy-Item -LiteralPath $f.FullName -Destination $part; Move-Item -LiteralPath $part -Destination $dest }
                catch { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue }
            }
        }
        # The tree goes at once, and the build's directory with it: what the release set keeps is in the artifact folder.
        try { $null = Remove-Trees $ctx } catch { "$(Get-Utc) trees: $($_.Exception.Message)" }
        if (-not $spec.keep) { Remove-Item -LiteralPath $d.work -Recurse -Force -ErrorAction SilentlyContinue }
        $result.seconds = [int]$watch.Elapsed.TotalSeconds
        Write-Json (Join-Path $logDir 'result.json') $result
    }
    $script:jobExit = $(if ($result.status -eq 'ok') { 0 } else { 1 })
}

if ($PSCmdlet.ParameterSetName -eq 'Job') {
    # The job's lines go to its job.log (this process's output); its result is result.json and the exit code.
    $script:jobExit = 1
    Invoke-SetJob $Job | Out-Host
    exit $script:jobExit
}

if ($PSCmdlet.ParameterSetName -eq 'Repack') {
    # 6. A notes-only repack (BD-265). The notes must be committed: the repack takes them from this tree, and a
    # published package names a commit for each of its texts.
    $From = [IO.Path]::GetFullPath($From)
    $python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
    $rr = [ordered]@{ tool = 'build-release-set'; step = 'repack-notes'; started_utc = Get-Utc; from = $From; flavours = @() }
    $rrFile = Join-Path $From 'repack-report.json'
    function Stop-Repack([string]$Why) {
        $rr.status = 'failed'; $rr.error = $Why; $rr.finished_utc = Get-Utc
        Write-Json $rrFile $rr
        "build-release-set -RepackNotes: $Why"
        exit 1
    }
    $notesDir = Join-Path $repo 'docs\testing\release-notes'
    if (@(& git -C $repo status --porcelain -- docs/testing/release-notes).Count) { Stop-Repack "the notes in $notesDir are not committed" }
    $rr.notes_commit = (& git -C $repo rev-parse HEAD).Trim()
    $signRoot = [IO.Path]::GetFullPath($PackageRoot)
    $secrets = $(if ($SigningSecrets) { [IO.Path]::GetFullPath($SigningSecrets) } else { Join-Path $signRoot 'secrets\release' })
    $signTool = Join-Path $signRoot 'toolchain\nuget\microsoft.windows.sdk.cpp\c\bin\10.0.26100.0\x64\signtool.exe'
    foreach ($flavour in @($Flavours | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
        $logs = Join-Path $From "logs\repack-$flavour"
        New-Item -ItemType Directory -Force $logs | Out-Null
        $newDir = Join-Path $From "package-notes\$flavour"
        "$(Get-Utc) repack $flavour (log $logs\repack.log)"
        # The signing inputs of build-release.ps1 (secrets\release, the SDK signtool): the repack signs the single exe
        # and, for the release flavour, the update manifest again (BD-294).
        & $shell -NoProfile -File (Join-Path $PSScriptRoot 'repack-notes.ps1') -Package (Join-Path $From "package\$flavour") -Out $newDir -NotesDir $notesDir `
            -SignTool $signTool -Pfx (Join-Path $secrets 'amdgpu-wddm-release.pfx') -PasswordFile (Join-Path $secrets 'amdgpu-wddm-release.pass') `
            -TestCertificate (Join-Path $secrets 'amdgpu-wddm-release.cer') *> (Join-Path $logs 'repack.log')
        if ($LASTEXITCODE -ne 0) {
            @(Get-Content -LiteralPath (Join-Path $logs 'repack.log') -Tail 15) | ForEach-Object { "    $_" }
            Stop-Repack "repack-notes.ps1 $flavour exit $LASTEXITCODE (log $logs\repack.log)"
        }
        $one = Get-Content -LiteralPath (Join-Path $newDir 'repack-report.json') -Raw | ConvertFrom-Json -AsHashtable
        $top = Join-Path $newDir ([IO.Path]::GetFileNameWithoutExtension($one.zip.file))
        # The checks of a package build on the new package: the flavour gate bound to its manifest (as build-release.ps1
        # runs it last) and the host dry run of the installer (as -Package runs it).
        & $python -I (Join-Path $repo 'tools\quality\flavour_gate.py') --flavour $flavour --package $top `
            --negative-control 'wow64/d3d12/amdgpu_wddm_vkd3d.dll' --out (Join-Path $logs 'flavour-gate') `
            --manifest (Join-Path $top 'files\manifest.json') *> (Join-Path $logs 'flavour-gate.log')
        if ($LASTEXITCODE -ne 0) { Stop-Repack "the flavour gate of the repacked $flavour package fails (log $logs\flavour-gate.log)" }
        & $shell -NoProfile -File (Join-Path $PSScriptRoot 'test-dryrun.ps1') -Package $top *> (Join-Path $logs 'dryrun.log')
        if ($LASTEXITCODE -ne 0) { Stop-Repack "the dry run of the repacked $flavour package fails (log $logs\dryrun.log)" }
        $rr.flavours += [ordered]@{ flavour = $flavour; zip = $one.zip; exe = $one.exe; unchanged_files = $one.unchanged_files; notes = $one.notes; update = $one.update }
        "$(Get-Utc) $flavour zip $($one.zip.new), exe $($one.exe.new), $($one.unchanged_files) files unchanged"
    }
    $rr.status = 'ok'; $rr.finished_utc = Get-Utc
    Write-Json $rrFile $rr
    "build-release-set -RepackNotes: ok (report $rrFile)"
    exit 0
}

if ($PSCmdlet.ParameterSetName -eq 'Package') {
    # 5. Packages. The input is the -Out of a release set run (-From) whose rows are registered: this tree's
    # tools\release\release-sources.json holds every row of <From>\release-sources.rows.json with the same source and
    # SHA-256, and the tree is clean and pushed, so the package is built from a commit that names its bytes. Then, for
    # each flavour: the setup window (tools\win\amdgpu_wddm_setup\build.ps1), the router inputs that the quality gates
    # read, and build-release.ps1 with the build records of the set (and the submission CAB for release).
    $Root = [IO.Path]::GetFullPath($PackageRoot)
    $From = [IO.Path]::GetFullPath($From)
    $env:BC250_ROOT = $Root
    $env:PYTHONDONTWRITEBYTECODE = '1'
    if (-not $PackageGuard) { $PackageGuard = Join-Path $Root 'scratch\cleanup-250\size-guard.py' }
    $python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
    $packageReport = if ($FlavourReport) { [IO.Path]::GetFullPath($FlavourReport) } else { Join-Path $From 'package-report.json' }
    $pr = [ordered]@{ tool = 'build-release-set'; step = 'package'; started_utc = Get-Utc; from = $From; flavours = @() }
    function Stop-Package([string]$Why) {
        $pr.status = 'failed'; $pr.error = $Why; $pr.finished_utc = Get-Utc
        Write-Json $packageReport $pr
        "build-release-set -Package: $Why"
        exit 1
    }
    if (@(& git -C $repo status --porcelain).Count) { Stop-Package "the recipe tree $repo is not clean: commit the registration first" }
    $head = (& git -C $repo rev-parse HEAD).Trim()
    if (-not @(& git -C $repo branch -r --contains $head).Count) { Stop-Package "the recipe tree's commit $head is on no remote-tracking branch: push it first" }
    $pr.recipe_commit = $head
    $setReport = Read-Json (Join-Path $From 'release-report.json')
    if ($setReport.status -ne 'ok') { Stop-Package "the release set in $From did not end well (status $($setReport.status))" }
    $setVersion = $setReport.version
    $pr.version = $setVersion
    # Every output of an earlier round that a step of this round refuses, in one message before the first step
    # (BD-272: tester.34 r7 stopped three times, each time minutes in, at the next output).
    Import-Module (Join-Path $PSScriptRoot 'PackageRound.psm1') -Force
    $earlierOutputs = @(Get-PackageRoundOutputs $From @($Flavours | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) $setVersion -Parallel:$Parallel)
    if ($earlierOutputs.Count) {
        if (-not $SetAside) { Stop-Package (Format-PackageRoundOutputs $earlierOutputs) }
        try { $setAsideRecord = Move-PackageRoundOutputs $From $earlierOutputs $SetAside }
        catch { Stop-Package "-SetAside ${SetAside}: $($_.Exception.Message)" }
        "$(Get-Utc) package: $($setAsideRecord.outputs.Count) output(s) of an earlier round moved to $($setAsideRecord.round) (set-aside.json)"
        $pr.set_aside = $setAsideRecord.round
    }
    $rowsDoc = Read-Json (Join-Path $From 'release-sources.rows.json')
    $registered = Read-Json (Join-Path $PSScriptRoot 'release-sources.json')
    $missing = @()
    foreach ($row in $rowsDoc.files) {
        $hit = @($registered.files | Where-Object { $_.path -ceq $row.path -and $_.flavour -ceq $row.flavour })
        if ($hit.Count -ne 1 -or $hit[0].sha256 -ne $row.sha256 -or $hit[0].source -ne $row.source) { $missing += "$($row.path) ($($row.flavour))" }
    }
    if ($missing.Count) { Stop-Package ("rows not registered in tools\release\release-sources.json (run the set with -Register, commit, push): " + ($missing -join ', ')) }
    "$(Get-Utc) package: $($rowsDoc.files.Count) rows registered at $($head.Substring(0, 8)), version $setVersion"
    # The work ledger gate of build-release.ps1, run here first with the same arguments: it refuses in seconds,
    # before the setup window and the package steps of each flavour spend about 10 minutes.
    $ledgerTool = Join-Path $Root 'scratch\ledger\ledger.py'
    if (Test-Path -LiteralPath $ledgerTool) {
        $ledgerOut = & $python $ledgerTool --root $Root check --release $setVersion --release-sources (Join-Path $PSScriptRoot 'release-sources.json') 2>&1
        $ledgerExit = $LASTEXITCODE
        if ($ledgerExit -gt 1) { Stop-Package "the work ledger checker failed with exit $ledgerExit (a malformed or missing LEDGER.json)" }
        if ($ledgerExit -ne 0 -and -not $LedgerDebtReason) {
            @($ledgerOut | Where-Object { "$_" -match '^\s*ERROR|error\(s\)' }) | ForEach-Object { "    $_" }
            Stop-Package 'the work ledger refuses this release (the lines above); land the work, or package with -LedgerDebtReason'
        }
        "$(Get-Utc) work ledger: exit $ledgerExit$(if ($ledgerExit) { ', debt reason given' })"
    }
    # Check every registered first-party PE, including rows carried from older
    # sets, before paying for setup, signing and catalog generation.
    foreach ($flavour in @($Flavours | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
        & $python -I (Join-Path $PSScriptRoot 'check_source_versions.py') `
            --sources (Join-Path $PSScriptRoot 'release-sources.json') --root $Root --repo $repo `
            --version $setVersion --flavour $flavour
        if ($LASTEXITCODE) { Stop-Package "source version resources failed ($flavour)" }
    }
    # The third-party list of the package names the commit of each payload file. build-release.ps1 checks it after
    # the setup window, minutes into the run; this is the same check before any of that (tester.37 package run 3).
    & $python -I (Join-Path $PSScriptRoot 'third_party_doc.py') --repo $repo --workspace $Root
    if ($LASTEXITCODE) { Stop-Package 'docs\testing\THIRD-PARTY.md does not name the commits of release-sources.json (the lines above; tools\release\third_party_doc.py --write, commit, push)' }
    foreach ($p in $UmdSource, $UmdBuild) { if (-not (Test-Path -LiteralPath $p)) { Stop-Package "user-mode driver tree missing: $p" } }
    $env:BC250_UMD_SOURCE = [IO.Path]::GetFullPath($UmdSource)
    $env:BC250_UMD_BUILD = [IO.Path]::GetFullPath($UmdBuild)
    function Get-Registered([string]$Path, [string]$Flavour) {
        $rows = @($registered.files | Where-Object { $_.path -ceq $Path -and $_.flavour -in @('shared', $Flavour) })
        if ($rows.Count -ne 1) { Stop-Package "$($rows.Count) registered rows of $Path ($Flavour), 1 expected" }
        $p = Join-Path $Root $rows[0].source
        if ((Get-FileHash -LiteralPath $p).Hash -ine $rows[0].sha256) { Stop-Package "$p does not match its registered sha256" }
        return $p
    }
    function Test-Guard([string]$What) {
        $guard = (& $python -I $PackageGuard 2>&1 | Select-Object -Last 1)
        "$(Get-Utc) $guard"
        if ($LASTEXITCODE -ne 0 -or $guard -notmatch 'size ok') { Stop-Package "the size guard refuses ${What}: $guard" }
    }
    $serial = @($Flavours | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    if ($Parallel -and $serial.Count -gt 1) {
        # One process and one detached worktree of $head per flavour. Each child runs the serial path below for its
        # flavour and writes its own report; the byte check of both runs here after every child ends.
        function Format-Arg([string]$A) { '"' + ($A -replace '"', '\"') + '"' }
        $children = @()
        # A package worktree of an earlier run whose folder was deleted stays registered (BD-264).
        foreach ($p in @(Remove-StaleWorktrees $null $repo $From)) { "$(Get-Utc) removed the registration of $p (its folder is gone)" }
        try {
            foreach ($flavour in $serial) {
                $wt = Join-Path $From "package-wt-$flavour"
                if (Test-Path -LiteralPath $wt) { Stop-Package "worktree exists: $wt (remove it with git worktree remove)" }
                & git -C $repo worktree add --detach $wt $head 2>&1 | Out-Null
                if ($LASTEXITCODE) { Stop-Package "git worktree add $wt failed" }
                $childReport = Join-Path $From "package-report-$flavour.json"
                Remove-Item -LiteralPath $childReport -ErrorAction SilentlyContinue
                $logs = Join-Path $From 'logs'
                New-Item -ItemType Directory -Force $logs | Out-Null
                $childArgs = @('-NoProfile', '-File', (Join-Path $wt 'tools\release\build-release-set.ps1'), '-Package',
                               '-From', $From, '-Flavours', $flavour, '-UmdSource', $env:BC250_UMD_SOURCE, '-UmdBuild', $env:BC250_UMD_BUILD,
                               '-PackageRoot', $Root, '-PackageGuard', $PackageGuard, '-FlavourReport', $childReport)
                if ($LedgerDebtReason) { $childArgs += @('-LedgerDebtReason', $LedgerDebtReason) }
                $packageLog = Join-Path $logs "package-$flavour.out"
                $p = Start-Process -FilePath $shell -ArgumentList (@($childArgs | ForEach-Object { Format-Arg $_ }) -join ' ') `
                    -WorkingDirectory $wt -NoNewWindow -PassThru -RedirectStandardOutput $packageLog -RedirectStandardError "$packageLog.err"
                "$(Get-Utc) package $flavour started in process $($p.Id) (worktree $wt, log $packageLog)"
                $children += [pscustomobject]@{ flavour = $flavour; process = $p; report = $childReport; out = $packageLog; wt = $wt }
            }
            foreach ($c in $children) {
                $c.process.WaitForExit()
                $cr = if (Test-Path -LiteralPath $c.report) { Read-Json $c.report } else { $null }
                if (-not $cr -or $cr.status -ne 'ok') {
                    @(Get-Content -LiteralPath $c.out -Tail 15 -ErrorAction SilentlyContinue) | ForEach-Object { "    $_" }
                    $why = if ($cr) { $cr.error } else { "no report $($c.report)" }
                    foreach ($o in $children) { if (-not $o.process.HasExited) { $o.process.WaitForExit() } }
                    Stop-Package "the $($c.flavour) package process failed: $why (log $($c.out))"
                }
                foreach ($cf in $cr.flavours) { $pr.flavours += $cf }
                "$(Get-Utc) package $($c.flavour) ended: $(@($cr.flavours | ForEach-Object { $_.zip.sha256 }) -join ', ')"
            }
        } finally {
            foreach ($c in $children) {
                if ($c.process.HasExited) { & git -C $repo worktree remove --force $c.wt 2>&1 | Out-Null }
            }
        }
        $serial = @()
    }
    foreach ($flavour in $serial) {
        $f = [ordered]@{ flavour = $flavour }
        $tmp = Join-Path $From "tmp-package-$flavour"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        $env:TEMP = $tmp; $env:TMP = $tmp
        $logs = Join-Path $From "logs\package-$flavour"
        New-Item -ItemType Directory -Force $logs | Out-Null
        # The setup window carries the package version in its resource (BD-166); an output of an earlier run is refused.
        $setup = Join-Path $From "setup\$flavour"
        if (Test-Path -LiteralPath $setup) { Stop-Package "setup output exists: $setup (remove it for a new package)" }
        # submission-cab.ps1 refuses an existing CAB only at the end of the package build (tester.35 r4 lost 25 minutes
        # to the CAB of r3), so it is refused here with the other outputs of an earlier run.
        if ($flavour -eq 'release') {
            $cabOut = Join-Path $From "submission\amdgpu-wddm-$setVersion.cab"
            if (Test-Path -LiteralPath $cabOut) { Stop-Package "submission CAB exists: $cabOut (remove it for a new package)" }
        }
        Test-Guard "the setup window ($flavour)"
        "$(Get-Utc) setup window $flavour"
        & $shell -NoProfile -File (Join-Path $repo 'tools\win\amdgpu_wddm_setup\build.ps1') -Out $setup -Version $setVersion -Flavour $flavour *> (Join-Path $logs 'setup.log')
        if ($LASTEXITCODE -ne 0) { Stop-Package "setup window $flavour exit $LASTEXITCODE (log $logs\setup.log)" }
        $setupExe = Join-Path $setup 'amdgpu_wddm_setup.exe'
        $stamp = (& $python -I (Join-Path $repo 'tools\build\versionstamp.py') show $setupExe 2>&1) -join "`n"
        if (-not $stamp.Contains("'ProductVersion': '$setVersion'")) { Stop-Package "the setup window $setupExe does not carry $setVersion" }
        $f.setup = [ordered]@{ file = $setupExe; sha256 = (Get-FileHash -LiteralPath $setupExe).Hash }
        # The router gate of the quality suite reads the hosted and CPU UMDs and the D3D11 app package of the flavour.
        $env:BC250_ROUTER_HOSTED_UMD = Get-Registered 'payload/desktop/bc250d3d_zink.dll' $flavour
        $env:BC250_ROUTER_CPU_UMD = Get-Registered 'payload/desktop/bc250d3d.dll' $flavour
        $app = Join-Path $From "router-inputs-$flavour"
        if (Test-Path -LiteralPath $app) { Remove-Item -LiteralPath $app -Recurse -Force }
        New-Item -ItemType Directory -Force $app | Out-Null
        foreach ($name in 'amdgpu_wddm_d3d11.dll', 'amdgpu_wddm_d3d11.config') {
            Copy-Item -LiteralPath (Get-Registered "payload/d3d11/$name" $flavour) -Destination (Join-Path $app $name)
        }
        $env:BC250_ROUTER_APP_PACKAGE = $app
        $pkgOut = Join-Path $From "package\$flavour"
        if (Test-Path -LiteralPath $pkgOut) { Stop-Package "package output exists: $pkgOut (remove it for a new package)" }
        Test-Guard "the $flavour package"
        $packageArgs = @('-Root', $Root, '-Out', $pkgOut, '-Version', $setVersion, '-Flavour', $flavour, '-SetupApp', $setup,
                         '-BuildRecords', (Join-Path $From 'artifacts'))
        if ($flavour -eq 'release') {
            $cab = Join-Path $From "submission\amdgpu-wddm-$setVersion.cab"
            New-Item -ItemType Directory -Force (Split-Path $cab) | Out-Null
            $packageArgs += @('-SubmissionCab', $cab)
        }
        if ($LedgerDebtReason) { $packageArgs += @('-AllowLedgerDebt', '-LedgerDebtReason', $LedgerDebtReason) }
        "$(Get-Utc) package $flavour (log $logs\build-release.log)"
        & $shell -NoProfile -File (Join-Path $PSScriptRoot 'build-release.ps1') @packageArgs *> (Join-Path $logs 'build-release.log')
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            @(Get-Content -LiteralPath (Join-Path $logs 'build-release.log') -Tail 15) | ForEach-Object { "    $_" }
            Stop-Package "build-release.ps1 $flavour exit $code (log $logs\build-release.log)"
        }
        # The driver package only: the self-update assets (nagi-apps-<version>.zip and the manifest) lie beside it.
        $zip = @(Get-ChildItem -LiteralPath $pkgOut -File -Filter 'amdgpu-wddm-tester-*.zip')
        if ($zip.Count -ne 1) { Stop-Package "$pkgOut holds $($zip.Count) driver package zips (amdgpu-wddm-tester-*.zip), 1 expected" }
        $f.zip = [ordered]@{ file = $zip[0].FullName; sha256 = (Get-FileHash -LiteralPath $zip[0].FullName).Hash; size = $zip[0].Length }
        if ($flavour -eq 'release') { $f.cab = [ordered]@{ file = $cab; sha256 = (Get-FileHash -LiteralPath $cab).Hash } }
        $f.top = Join-Path $pkgOut ([IO.Path]::GetFileNameWithoutExtension($zip[0].Name))
        # The host dry run of the installer on the unpacked package.
        "$(Get-Utc) dry run $flavour (log $logs\dryrun.log)"
        & $shell -NoProfile -File (Join-Path $PSScriptRoot 'test-dryrun.ps1') -Package $f.top *> (Join-Path $logs 'dryrun.log')
        if ($LASTEXITCODE -ne 0) { Stop-Package "the dry run of the $flavour package fails (log $logs\dryrun.log)" }
        $pr.flavours += $f
        "$(Get-Utc) $flavour $($zip[0].Name) $($f.zip.sha256) $($f.zip.size) bytes"
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
    # BD-167: no lab-only string in a release Mesa DLL, and each one its tree holds in the lab DLL of the same slot.
    $tops = @{}; foreach ($f in $pr.flavours) { $tops[$f.flavour] = $f.top }
    if ($tops.release -and $tops.lab) {
        $mesa = @($registered.repositories['mesa-amdgpu-wddm'].checkouts | ForEach-Object { Join-Path $Root $_ } |
                  Where-Object { Test-Path -LiteralPath $_ }) | Select-Object -First 1
        if (-not $mesa) { Stop-Package 'no checkout of mesa-amdgpu-wddm for the flavour byte check' }
        & $python -I (Join-Path $PSScriptRoot 'flavour_bytes.py') --release (Join-Path $tops.release 'files') `
            --lab (Join-Path $tops.lab 'files') --sources (Join-Path $PSScriptRoot 'release-sources.json') --mesa $mesa *> (Join-Path $From 'logs\flavour-bytes.log')
        if ($LASTEXITCODE -ne 0) { Stop-Package "the flavour byte check fails (log $From\logs\flavour-bytes.log)" }
        $pr.flavour_bytes = 'PASS'
    }
    $pr.status = 'ok'; $pr.finished_utc = Get-Utc
    Write-Json $packageReport $pr
    "build-release-set -Package: ok (report $packageReport)"
    exit 0
}

# ---------------------------------------------------------------------------------------------------------------
# The release set.
$started = [DateTime]::UtcNow
$Root = [IO.Path]::GetFullPath($Root)
$Out = [IO.Path]::GetFullPath($Out)
$Manifest = [IO.Path]::GetFullPath($Manifest)
if (-not $Work) { $Work = Join-Path $Root 'scratch\repro-builds\bt' }
$Work = [IO.Path]::GetFullPath($Work)
if (-not $PackageCache) { $PackageCache = Join-Path $Root 'scratch\repro-builds\packagecache' }
if (-not $SizeGuard) { $SizeGuard = Join-Path $Root 'scratch\cleanup-250\size-guard.py' }
$python = (Get-Command python -CommandType Application | Select-Object -First 1).Source
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$tools = @{ kits = (Join-Path $Root 'toolchain\nuget') }
foreach ($t in @($Tool | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
    $k, $v = $t -split '=', 2
    if (-not $v) { throw "-Tool takes name=path, not '$t'" }
    $tools[$k.ToLowerInvariant()] = $v
}
New-Item -ItemType Directory -Force $Out | Out-Null
$planFile = Join-Path $Out 'plan.json'
$reportFile = Join-Path $Out 'release-report.json'
$rowsFile = Join-Path $Out 'release-sources.rows.json'

"$(Get-Utc) build-release-set: $Manifest"
$planArgs = @($buildReleaseSet, 'plan', '--manifest', $Manifest, '--root', $Root, '--repo', $repo, '--out', $Out, '--work', $Work,
    '--max-parallel', $MaxParallel, '--jobs', $Jobs, '--write', $planFile)
if ($Sources) { $planArgs += @('--sources', [IO.Path]::GetFullPath($Sources)) }
if ($Only.Count) { $planArgs += @('--only', ($Only -join ',')) }
if ($Version) { $planArgs += @('--version', $Version) }
if ($KeepFrom) { $planArgs += @('--keep-from', [IO.Path]::GetFullPath($KeepFrom)) }
& $python @planArgs
if ($LASTEXITCODE -ne 0) { "build-release-set: the manifest does not plan (see above)"; exit 1 }
$planData = Read-Json $planFile
$lock = 'Local\bc250-build-release-set-' + [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Work.ToLowerInvariant()))).Replace('-', '').Substring(0, 12)
$ctx = New-BuildContext -Sources $Manifest -Root $Root -Repo $repo -Tools $tools -Lock $lock

# The parameters of each recipe at its commit, as PowerShell parses them, for the compiler gate.
$params = [ordered]@{}
foreach ($r in $planData.recipes) {
    $key = "$($r.commit):$($r.recipe)"
    if (-not $r.recipe.EndsWith('.ps1')) { $params[$key] = @{ params = @() }; continue }
    try {
        $checkout = Get-Checkout $ctx $r.repo $r.commit
        $text = (& git -C $checkout show "$($r.commit):$($r.recipe)") -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "git show $($r.commit):$($r.recipe) failed" }
        $tokens = $null; $parseErrors = $null
        $ast = [Management.Automation.Language.Parser]::ParseInput($text, [ref]$tokens, [ref]$parseErrors)
        $names = @(if ($ast.ParamBlock) { foreach ($p in $ast.ParamBlock.Parameters) { $p.Name.VariablePath.UserPath } })
        $params[$key] = @{ params = $names }
    } catch {
        $params[$key] = @{ params = $null; error = $_.Exception.Message }
    }
}
$paramsFile = Join-Path $Out 'recipe-params.json'
Write-Json $paramsFile $params
$gatesFile = Join-Path $Out 'gates.json'
$gateArgs = @($buildReleaseSet, 'gates', '--plan', $planFile, '--recipe-params', $paramsFile, '--write', $gatesFile)
if ($AllowUnpushed) { $gateArgs += '--allow-unpushed' }
& $python @gateArgs
$gates = Read-Json $gatesFile
$gateList = [Collections.Generic.List[object]]::new()
foreach ($g in $gates.gates) { $gateList.Add($g) }

# The size guard: its record when it is young enough, else a new measurement (about a minute on this workspace).
function Get-SizeMeasure([switch]$Fresh) {
    $record = Join-Path (Split-Path $SizeGuard) 'size-latest.json'
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $line = $null; $code = 0; $ran = $false
    $r = if (Test-Path -LiteralPath $record) { Read-Json $record } else { $null }
    if ($Fresh -or -not $r -or ($now - [long]$r.epoch) -gt $GuardMaxAge) {
        $text = @(& $python -I $SizeGuard 2>&1 | ForEach-Object { [string]$_ })
        $code = $LASTEXITCODE; $ran = $true
        $line = ($text | Select-Object -Last 1)
        $r = if (Test-Path -LiteralPath $record) { Read-Json $record } else { $null }
        if (-not $r -or [long]$r.epoch -lt $now - 5) { throw "size guard ${SizeGuard}: no new record ($line)" }
    }
    return [pscustomobject]@{ counted = [long]$r.counted_bytes; warn = [long]$r.warn_line_bytes; free = [long]$r.free_bytes
        free_warn = [long]$r.free_warn_bytes; utc = $(if ($r.utc -is [datetime]) { $r.utc.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') } else { [string]$r.utc })
        epoch = [long]$r.epoch; ran = $ran; code = $code; line = $line
        taken = [DateTime]::UtcNow }
}
function Format-GB([long]$Bytes) { [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.0} GB', $Bytes / 1e9) }

$measures = [Collections.Generic.List[object]]::new()
try {
    $measure = Get-SizeMeasure
    $measures.Add($measure)
    $sizeOk = $measure.code -eq 0 -and $measure.counted -lt $measure.warn -and $measure.free -gt $measure.free_warn
    $gateList.Add([ordered]@{ name = 'size'; status = $(if ($sizeOk) { 'PASS' } else { 'FAIL' })
            lines = @("counted $(Format-GB $measure.counted) of the warning line $(Format-GB $measure.warn), free $(Format-GB $measure.free) above $(Format-GB $measure.free_warn), measured $($measure.utc)$(if (-not $measure.ran) { ' (the record of the last run)' })") })
} catch {
    $measure = $null
    $gateList.Add([ordered]@{ name = 'size'; status = 'FAIL'; lines = @($_.Exception.Message) })
}
$foreign = @(Get-Process -Name ninja, clang-cl, cl, lld-link, link, meson -ErrorAction SilentlyContinue)
$gateList.Add([ordered]@{ name = 'host'; status = $(if ($foreign.Count) { 'WARN' } else { 'PASS' })
        lines = @($(if ($foreign.Count) { "$($foreign.Count) compiler processes of other builds run now ($(($foreign | Group-Object Name | ForEach-Object { "$($_.Count) $($_.Name)" }) -join ', ')): they share the cores" } else { 'no other compiler runs' })) })
# The KMD recipe runs the fast quality gates (tools\quality\quick.ps1), which read the user-mode driver tree of the
# release from BC250_UMD_SOURCE and BC250_UMD_BUILD. Without them the KMD build fails minutes into the run and stops
# every other build (tester.37, 2026-11-04), so the gate refuses the set before the first build.
$kmdRows = @($planData.builds | Where-Object { $_.recipe -eq 'driver/kmd/build.ps1' -or $_.recipe -eq 'driver\kmd\build.ps1' })
if ($kmdRows.Count) {
    $umdLines = [Collections.Generic.List[string]]::new()
    foreach ($n in 'BC250_UMD_SOURCE', 'BC250_UMD_BUILD') {
        $v = [Environment]::GetEnvironmentVariable($n)
        if (-not $v) { $umdLines.Add("$n is not set: set it to the umd-gate tree of the release (Mesa at the zink-umd row commit, and its configured build)") }
        elseif (-not (Test-Path -LiteralPath $v -PathType Container)) { $umdLines.Add("$n=$v is not a folder") }
    }
    $gateList.Add([ordered]@{ name = 'umd-gate'; status = $(if ($umdLines.Count) { 'FAIL' } else { 'PASS' })
            lines = @($(if ($umdLines.Count) { $umdLines } else { "BC250_UMD_SOURCE=$env:BC250_UMD_SOURCE, BC250_UMD_BUILD=$env:BC250_UMD_BUILD, read by $($kmdRows.Count) KMD build(s)" })) })
}
# The work ledger gate that -Package runs, in release mode, before the first build: work deferred to this very release
# or unlanded work that serves it refuses the package, and tester.37 found that only after its 78-minute set build.
$ledgerTool = Join-Path $Root 'scratch\ledger\ledger.py'
if (Test-Path -LiteralPath $ledgerTool) {
    $ledgerOut = @(& $python $ledgerTool --root $Root check --release $planData.version 2>&1 | ForEach-Object { [string]$_ })
    $ledgerExit = $LASTEXITCODE
    $ledgerLines = @($ledgerOut | Where-Object { $_ -match '^\s*ERROR|error\(s\)' } | ForEach-Object { $_.Trim() })
    $gateList.Add([ordered]@{ name = 'ledger'; status = $(if ($ledgerExit -eq 0) { 'PASS' } else { 'FAIL' })
            lines = @($(if ($ledgerLines.Count) { $ledgerLines } else { "ledger.py check --release $($planData.version): exit $ledgerExit" })) })
}
foreach ($g in @($gateList | Select-Object -Skip $gates.gates.Count)) {
    "gate $($g.name): $($g.status)"
    foreach ($l in $g.lines) { "  $l" }
}
$gatesPass = -not @($gateList | Where-Object { $_.status -eq 'FAIL' }).Count

# The builds, as they would run.
$byId = @{}
foreach ($b in $planData.builds) { $byId[$b.id] = $b }
$keptById = @{}
foreach ($k in $planData.kept) { $keptById[$k.id] = $k }
function Get-PayloadMap($Build, $Results) {
    $m = @{}
    foreach ($path in @($Build.payload_from.Keys)) {
        $from = $Build.payload_from[$path]
        if ($from.file) { $m[$path] = $from.file; continue }
        $dep = $from.row
        $res = if ($Results -and $Results.ContainsKey($dep)) { $Results[$dep] } elseif ($keptById.ContainsKey($dep)) { $keptById[$dep] } else { $null }
        $hit = if ($res) { @($res.outputs | Where-Object { $_.path -eq $path }) | Select-Object -First 1 } else { $null }
        $m[$path] = if ($hit) { $hit.artifact } else {
            $def = $byId[$dep]
            $file = @($def.outputs | Where-Object { $_.path -eq $path })[0].file
            Join-Path $def.artifact (Split-Path ($file -replace '/', '\') -Leaf)
        }
    }
    return $m
}
"build-release-set: $($planData.builds.Count) builds$(if ($planData.kept.Count) { ", $($planData.kept.Count) kept from the last run" }), -MaxParallel $MaxParallel, $($planData.jobs_per_slot) jobs per slot ($($planData.logical_processors) logical processors), work $Work"
if ($planData.changed) { foreach ($p in $planData.changed.GetEnumerator()) { "  build $($p.Key): $($p.Value)" } }
foreach ($k in $planData.kept) { "  kept $($k.id):$(@($k.outputs | ForEach-Object { "$($_.sha256.Substring(0, 8)) $($_.path)" }) -join ', ')$(if ($k.imported_from) { " (imported from $($k.imported_from.artifact))" })" }
# The builds of the report: the kept rows, then the result of each planned row. A planned row that did not start keeps
# its earlier ok build (planData.carried): this run did not touch its artifact, and the next run in -Out must still
# find it (BD-244). A kept row that was to be imported and was not (a run that ended before the import) is left out,
# because its files are not in -Out.
function Copy-Entry($Entry) { $e = [ordered]@{}; foreach ($key in $Entry.Keys) { $e[$key] = $Entry[$key] }; return $e }
function Get-ReportBuilds([hashtable]$Results, [switch]$NotImported) {
    $carried = @{}
    foreach ($c in @($planData.carried)) { if ($c) { $carried[$c.id] = $c } }
    $list = @(foreach ($k in $planData.kept) {
        if ($NotImported -and $k.imported_from) { continue }
        $e = Copy-Entry $k; $e.kept = $true; $e
    })
    $list += @(foreach ($b in $planData.builds) {
        $r = if ($Results) { $Results[$b.id] } else { $null }
        if ((-not $r -or $r.status -in 'not-started', 'skipped') -and $carried.ContainsKey($b.id)) {
            $e = Copy-Entry $carried[$b.id]; $e.carried = $true
            $e.carried_reason = "this run did not build the row$(if ($r) { " ($($r.status): $($r.error))" })"
            $e
        } else { $r ?? [ordered]@{ id = $b.id; status = 'not-started'; definition = $b } }
    })
    return $list
}
$index = 0
foreach ($b in $planData.builds) {
    $index++
    $ctx.SourceOf = Get-PayloadMap $b $null
    $inputMap = Resolve-Inputs $ctx $b.inputs $b.dirs.inputs -NoCreate
    $map = New-ArgMap $b $Root $planData.version $inputMap
    $expanded = @(foreach ($a in @($b.args)) { try { Expand-Arg $ctx $a $map } catch { "<$($_.Exception.Message)>" } })
    "[$index/$($planData.builds.Count)] $($b.id): $($b.component) $($b.flavour) $($b.arch) $($b.compiler), $($b.repo) $($b.commit.Substring(0, 8)) ($($b.tree)), recipe $($b.recipe_commit.Substring(0, 8)), $($b.slots) slot(s), $($b.jobs) jobs, peak $(Format-GB $b.disk_peak_bytes)$(if ($b.deps.Count) { ", after $($b.deps -join ', ')" })$(if (-not $b.deterministic) { ', unverified' })"
    "    $($b.dirs.recipe)\$($b.recipe -replace '/', '\') $($expanded -join ' ')"
    foreach ($o in $b.outputs) { "    -> $($o.path) ($(if ($o.flavour) { $o.flavour } else { $b.flavour })) = out\$($o.file -replace '/', '\')" }
    foreach ($t in @($b.tests)) { if ($t) { "    test $($t.name): $($t.run) $(@($t.args) -join ' ')$(if ($t.requires) { "  (when $($t.requires) exists)" })" } }
}
foreach ($s in $planData.shared_trees) { "  shared tree $($s.repo) $($s.commit.Substring(0, 8)) at $($s.path), read by $($s.rows -join ', ')" }

$report = [ordered]@{ tool = 'build-release-set'; schema = 1; release = $planData.release; version = $planData.version; manifest = $Manifest
    manifest_sha256 = $planData.manifest_sha256; root = $Root; out = $Out; work = $Work; only = $Only; plan = [bool]$Plan
    started_utc = $started.ToString('yyyy-MM-ddTHH:mm:ssZ'); finished_utc = $null; seconds = 0; status = $null
    host = [ordered]@{ logical_processors = $planData.logical_processors; max_parallel = $MaxParallel; jobs_per_slot = $planData.jobs_per_slot }
    gates = $gateList; disk = [ordered]@{ measurements = $measures; peak_used_estimate = [long]0 }; builds = @(); rows = $null }
if ($Plan) {
    "build-release-set: -Plan, nothing built; gates $(if ($gatesPass) { 'PASS' } else { 'FAIL' })"
    exit $(if ($gatesPass) { 0 } else { 1 })
}
if (-not $gatesPass) {
    $report.status = 'gates-failed'; $report.finished_utc = Get-Utc
    $report.builds = @(Get-ReportBuilds $null -NotImported)
    Write-Json $reportFile $report
    "build-release-set: a gate fails, so nothing is built (report $reportFile)"
    exit 1
}
if (@($planData.kept | Where-Object { $_.imported_from }).Count) {
    & $python $buildReleaseSet import --plan $planFile
    if ($LASTEXITCODE -ne 0) {
        $report.status = 'import-failed'; $report.finished_utc = Get-Utc
        $report.builds = @(Get-ReportBuilds $null -NotImported)
        Write-Json $reportFile $report
        "build-release-set: the import of the -KeepFrom builds fails, so nothing is built (report $reportFile)"
        exit 1
    }
    # The import writes the digests of the files it stamped with this version into the plan (build_release_set.py).
    $planData = Read-Json $planFile
    $keptById = @{}
    foreach ($k in $planData.kept) { $keptById[$k.id] = $k }
    "$(Get-Utc) imported $(@($planData.kept | Where-Object { $_.imported_from }).Count) kept builds from $($planData.keep_from)"
}

# ---------------------------------------------------------------------------------------------------------------
# The scheduler.
$results = @{}            # id -> report entry of a build that ended (this run)
$state = @{}              # id -> pending | running | ok | failed | skipped | not-started
$running = @{}            # id -> process record
foreach ($b in $planData.builds) { $state[$b.id] = 'pending' }
foreach ($k in $planData.kept) { $results[$k.id] = $k }
$sharedBytes = [long]0
$stopping = $false
$failedAny = $false
$diskFail = $null
$lastSample = [DateTime]::MinValue
$sinceMeasure = [long]0     # bytes the finished builds left after the last measurement (their artifacts)

# What the builds may still add, counted from the last measurement: each running build may grow to its peak, and
# what it occupied at the measurement is in the measurement already.
function Get-DiskUse {
    $reserved = [long]0
    foreach ($r in $running.Values) { $reserved += [math]::Max([long]0, $r.build.disk_peak_bytes - $r.at_measure) }
    return $measure.counted + $sharedBytes + $sinceMeasure + $reserved
}
function Test-Fits($Build) {
    $used = Get-DiskUse
    $free = $measure.free - ($used - $measure.counted)
    return ($used + $Build.disk_peak_bytes -le $measure.warn) -and ($free - $Build.disk_peak_bytes -ge $measure.free_warn)
}
function Update-Measure {
    $script:measure = Get-SizeMeasure -Fresh
    $measures.Add($script:measure)
    foreach ($r in $running.Values) { $r.at_measure = Get-TreeBytes $r.build.dirs.work }
    $script:sharedBytes = [long]0
    $script:sinceMeasure = [long]0
    "$(Get-Utc) size guard: counted $(Format-GB $measure.counted), free $(Format-GB $measure.free)"
}

try {
    # Registrations of trees under -Work whose folder is gone (a -Keep run whose Work folder was deleted, a killed
    # job): git worktree add refuses their paths. Only those of this -Work go, never those of another lane (BD-264).
    $treeRepos = @(foreach ($b in $planData.builds) {
        , @($b.repo, $b.commit); , @($planData.recipe_repository, $b.recipe_commit)
        foreach ($i in @(if ($b.inputs) { $b.inputs.Values })) { if ($i.repo -and $i.commit) { , @($i.repo, $i.commit) } }
    })
    $seen = @{}; $checkouts = @{}
    foreach ($pair in $treeRepos) {
        $key = "$($pair[0])@$($pair[1])"
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $checkouts[(Get-Checkout $ctx $pair[0] $pair[1])] = $true
    }
    foreach ($c in $checkouts.Keys) {
        foreach ($p in @(Remove-StaleWorktrees $ctx $c $Work)) { "$(Get-Utc) removed the registration of $p in $c (its folder is gone)" }
    }
    # The shared trees, once, before the builds that read them.
    foreach ($s in $planData.shared_trees) {
        Remove-SharedBuildTree $ctx $s.repo $s.commit $s.path $Work
        New-Tree $ctx $s.repo $s.commit $s.path
        $sharedBytes += Get-TreeBytes $s.path
    }
    while ($true) {
        # Builds that ended.
        foreach ($id in @($running.Keys)) {
            $r = $running[$id]
            $timedOut = ([DateTime]::UtcNow - $r.started).TotalSeconds -gt $BuildTimeout
            if (-not $r.process.HasExited -and -not $timedOut) { continue }
            if ($timedOut -and -not $r.process.HasExited) {
                & taskkill.exe /PID $r.process.Id /T /F 2>&1 | Out-Null
                $r.process.WaitForExit()
            }
            $running.Remove($id)
            $resultFile = Join-Path $r.build.log_dir 'result.json'
            $res = if (Test-Path -LiteralPath $resultFile) { Read-Json $resultFile } else { [ordered]@{ id = $id; status = 'failed'; error = 'the job wrote no result (see job.log)'; outputs = @() } }
            if ($timedOut) { $res.status = 'failed'; $res.error = "stopped after -BuildTimeout $BuildTimeout s" }
            $res.peak_bytes = [math]::Max($r.peak, [long]0)
            $res.slots = $r.build.slots
            $res.jobs = $r.build.jobs
            $res.started_utc = $r.started.ToString('yyyy-MM-ddTHH:mm:ssZ')
            $res.finished_utc = Get-Utc
            $res.log_dir = $r.build.log_dir
            $res.definition = $r.build
            $results[$id] = $res
            $state[$id] = $res.status
            $art = Get-TreeBytes $r.build.artifact
            $sinceMeasure += $art - $r.at_measure
            "$(Get-Utc) end $id`: $($res.status) after $([int]([DateTime]::UtcNow - $r.started).TotalSeconds) s, peak $(Format-GB $res.peak_bytes)$(if ($res.error) { ": $($res.error)" })"
            foreach ($o in @($res.outputs)) { "    $($o.sha256.Substring(0, 8)) $($o.size) $($o.path) ($($o.flavour))" }
            if ($res.status -ne 'ok') {
                $failedAny = $true
                if (-not $KeepGoing) { $stopping = $true }
            }
        }
        # A build whose dependency failed does not run.
        foreach ($b in $planData.builds) {
            if ($state[$b.id] -ne 'pending') { continue }
            $bad = @($b.deps | Where-Object { $state.ContainsKey($_) -and $state[$_] -in 'failed', 'skipped', 'not-started' })
            if ($bad.Count) { $state[$b.id] = 'skipped'; $results[$b.id] = [ordered]@{ id = $b.id; status = 'skipped'; error = "needs $($bad -join ', '), which did not build"; definition = $b }; "$(Get-Utc) skip $($b.id): needs $($bad -join ', ')" }
        }
        if ($stopping) {
            foreach ($b in $planData.builds) { if ($state[$b.id] -eq 'pending') { $state[$b.id] = 'not-started'; $results[$b.id] = [ordered]@{ id = $b.id; status = 'not-started'; error = 'a build of this release set failed first'; definition = $b } } }
        }
        # New builds, in plan order.
        $slotsUsed = 0
        foreach ($r in $running.Values) { $slotsUsed += $r.build.slots }
        foreach ($b in $planData.builds) {
            if ($stopping -or $diskFail) { break }
            if ($state[$b.id] -ne 'pending') { continue }
            if (@($b.deps | Where-Object { $state.ContainsKey($_) -and $state[$_] -ne 'ok' }).Count) { continue }
            if ($slotsUsed + $b.slots -gt $MaxParallel) { continue }
            if (-not (Test-Fits $b)) {
                if (([DateTime]::UtcNow - $measure.taken).TotalSeconds -gt $GuardMaxAge -or -not $running.Count) { Update-Measure }
                if (-not (Test-Fits $b)) {
                    if (-not $running.Count) {
                        $diskFail = "$($b.id) needs $(Format-GB $b.disk_peak_bytes) at its peak, and the size guard leaves $(Format-GB ([math]::Min($measure.warn - (Get-DiskUse), $measure.free - $measure.free_warn))) under its warning lines"
                        "$(Get-Utc) FAIL disk: $diskFail"
                        $failedAny = $true
                    }
                    # Plan order: a later build does not overtake one that waits for room.
                    break
                }
            }
            $ctx.SourceOf = Get-PayloadMap $b $results
            $logDir = $b.log_dir
            if (Test-Path -LiteralPath $logDir) { Remove-Item -LiteralPath $logDir -Recurse -Force }
            New-Item -ItemType Directory -Force $logDir | Out-Null
            if (Test-Path -LiteralPath $b.artifact) { Remove-Item -LiteralPath $b.artifact -Recurse -Force }
            $jobFile = Join-Path $logDir 'job.json'
            Write-Json $jobFile ([ordered]@{ build = $b; manifest = $Manifest; root = $Root; repo = $repo; lock = $lock; tools = $tools
                    payload = $ctx.SourceOf; version = $planData.version; package_cache = $PackageCache; python = $python
                    build_release_set = $buildReleaseSet; recipe_repository = $planData.recipe_repository; keep = [bool]$Keep; log_dir = $logDir })
            $argLine = "-NoProfile -File `"$PSCommandPath`" -Job `"$jobFile`""
            $p = Start-Process -FilePath $shell -ArgumentList $argLine -NoNewWindow -PassThru `
                -RedirectStandardOutput (Join-Path $logDir 'job.log') -RedirectStandardError (Join-Path $logDir 'job-error.log')
            $null = $p.Handle     # keeps the handle, so the exit code is there after the process ends
            $running[$b.id] = [pscustomobject]@{ build = $b; process = $p; started = [DateTime]::UtcNow; peak = [long]0; at_measure = [long]0 }
            $state[$b.id] = 'running'
            $slotsUsed += $b.slots
            "$(Get-Utc) start $($b.id) ($($b.slots) slot(s), $($b.jobs) jobs, peak $(Format-GB $b.disk_peak_bytes), disk $(Format-GB (Get-DiskUse)) of $(Format-GB $measure.warn) reserved)"
            $report.disk.peak_used_estimate = [math]::Max([long]$report.disk.peak_used_estimate, [long](Get-DiskUse))
        }
        if (-not $running.Count -and ($diskFail -or -not @($state.Values | Where-Object { $_ -eq 'pending' }).Count)) { break }
        # The size of each running build, at most every 15 s: the report's peak, and the reservation after a measurement.
        if (([DateTime]::UtcNow - $lastSample).TotalSeconds -ge 15) {
            foreach ($r in $running.Values) { $r.peak = [math]::Max($r.peak, (Get-TreeBytes $r.build.dirs.work)) }
            $lastSample = [DateTime]::UtcNow
        }
        Start-Sleep -Milliseconds 500
    }
} finally {
    # Ctrl+C, a timeout or an error of this script: the builds it started stop with it, and the shared trees go.
    foreach ($r in $running.Values) {
        if (-not $r.process.HasExited) { & taskkill.exe /PID $r.process.Id /T /F 2>&1 | Out-Null }
    }
    if (-not $Keep) {
        try { Remove-Trees $ctx } catch { "trees: $($_.Exception.Message)" }
        if ((Test-Path -LiteralPath $Work) -and -not @(Get-ChildItem -LiteralPath $Work -Force).Count) { Remove-Item -LiteralPath $Work -Force }
    }
}

# ---------------------------------------------------------------------------------------------------------------
# The report and the rows.
$report.builds = @(Get-ReportBuilds $results)
$report.finished_utc = Get-Utc
$report.seconds = [int]([DateTime]::UtcNow - $started).TotalSeconds
$report.status = if ($failedAny) { 'failed' } else { 'ok' }
if ($diskFail) { $report.disk.failure = $diskFail }
Write-Json $reportFile $report
$rowArgs = @($buildReleaseSet, 'rows', '--plan', $planFile, '--report', $reportFile, '--write', $rowsFile)
if ($Compare) { $rowArgs += @('--compare', [IO.Path]::GetFullPath($Compare)) }
if ($MergeInto) { $rowArgs += @('--merge', [IO.Path]::GetFullPath($MergeInto), '--merged', (Join-Path $Out 'release-sources.json')) }
& $python @rowArgs
$rowsCode = $LASTEXITCODE
$rowsDoc = Read-Json $rowsFile
$report.rows = [ordered]@{ file = $rowsFile; count = @($rowsDoc.files).Count; compare = $rowsDoc.compare; notes = $rowsDoc.notes }
if ($rowsCode -ne 0) { $report.status = 'failed' }
if ($Register -and $report.status -eq 'ok') {
    # The registration edits this tree's release-sources.json; the package step (-Package) needs it committed and pushed.
    & $python $buildReleaseSet register --rows $rowsFile --sources (Join-Path $PSScriptRoot 'release-sources.json') --root $Root `
        --repo $repo --configs-out (Join-Path $Out 'configs') --receipt (Join-Path $Out 'register-receipt.json')
    if ($LASTEXITCODE -ne 0) { $report.status = 'failed' }
    $report.register = Join-Path $Out 'register-receipt.json'
    # The third-party list names the commit of each payload file, so the new rows change it. -Package refuses a list
    # that does not match release-sources.json; write it here, so that the same commit carries both files.
    if ($report.status -eq 'ok') {
        & $python -I (Join-Path $PSScriptRoot 'third_party_doc.py') --repo $repo --workspace $Root --write
        if ($LASTEXITCODE -ne 0) { $report.status = 'failed' }
    }
}
Write-Json $reportFile $report
"build-release-set: $($report.status), $(@($report.builds | Where-Object { $_.status -eq 'ok' }).Count) of $($report.builds.Count) builds ok, $($report.rows.count) rows in $rowsFile, $($report.seconds) s (report $reportFile)"
exit $(if ($report.status -eq 'ok') { 0 } else { 1 })
