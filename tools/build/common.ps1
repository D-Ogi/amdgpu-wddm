# Shared helpers for build-llvm.ps1, build-mesa.ps1, build-dxvk.ps1 and build-vkd3d.ps1. Dot-sourced by them, not
# a command of its own.
#
# Only the build environment lives here: the workspace root, the Visual Studio developer environment, tool
# versions, source identity and the recipe.json record. The recipes themselves (options, targets) stay in the
# two scripts and in mesa-configs.json, so that one file answers "how was this built".

Set-StrictMode -Version 3.0

# BC250_ROOT when set, else the directory that holds the repository (docs/build.md, "Workspace layout").
function Get-Bc250Root([string]$Repo) {
    if ($env:BC250_ROOT) { return [IO.Path]::GetFullPath($env:BC250_ROOT) }
    return Split-Path -Parent $Repo
}

# The recipes change PATH, INCLUDE, TEMP and friends. Whoever runs one from an interactive session gets the
# session back unchanged, success or failure.
function Save-ProcessEnvironment {
    $saved = @{}
    foreach ($e in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) { $saved[[string]$e.Key] = [string]$e.Value }
    return $saved
}

function Restore-ProcessEnvironment([hashtable]$Saved) {
    foreach ($name in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
        if (-not $Saved.ContainsKey([string]$name)) { [Environment]::SetEnvironmentVariable([string]$name, $null, 'Process') }
    }
    foreach ($name in $Saved.Keys) { [Environment]::SetEnvironmentVariable($name, $Saved[$name], 'Process') }
}

function Add-PathFront([string[]]$Directories) {
    $front = @($Directories | Where-Object { $_ })
    if ($front.Count) { $env:PATH = ($front + @($env:PATH)) -join ';' }
}

function Find-VsInstall([string]$VsInstall) {
    if ($VsInstall) {
        if (-not (Test-Path -LiteralPath (Join-Path $VsInstall 'VC\Auxiliary\Build\vcvars64.bat'))) {
            throw "-VsInstall $VsInstall has no VC\Auxiliary\Build\vcvars64.bat"
        }
        return $VsInstall
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found at ${vswhere}. Install Visual Studio 2022 or its Build Tools with the C++ workload, or pass -VsInstall"
    }
    $found = @(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $found.Count -or -not $found[0]) { throw 'vswhere found no Visual Studio instance with the x64 C++ tools' }
    return $found[0]
}

# The recorded builds ran from a cmd script that called vcvars64.bat first. This is the same thing for a
# PowerShell process: run vcvars64.bat in cmd, then copy the resulting environment into this process.
# -Arch x86 runs vcvarsamd64_x86.bat instead (x64-hosted compiler for 32-bit targets): the WoW64 user-mode drivers
# that 32-bit processes load. Meson takes the target from VSCMD_ARG_TGT_ARCH and treats it as a native x86 build.
function Import-VsDevEnvironment([string]$VsInstall, [string]$TempDir, [ValidateSet('x64', 'x86')][string]$Arch = 'x64') {
    $install = Find-VsInstall $VsInstall
    $vcvars = Join-Path $install ('VC\Auxiliary\Build\{0}' -f @{ x64 = 'vcvars64.bat'; x86 = 'vcvarsamd64_x86.bat' }[$Arch])
    New-Item -ItemType Directory -Force $TempDir | Out-Null
    $probe = Join-Path $TempDir ('vsenv-{0}.cmd' -f [Guid]::NewGuid().ToString('N'))
    Set-Content -LiteralPath $probe -Encoding ascii -Value @(
        '@echo off',
        'set VSCMD_SKIP_SENDTELEMETRY=1',
        "call `"$vcvars`" >nul",
        'if errorlevel 1 exit /b 1',
        'set')
    try {
        $lines = @(& cmd.exe /d /c $probe)
        if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $vcvars) failed ($LASTEXITCODE): $vcvars" }
    } finally {
        Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
    }
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $eq), $line.Substring($eq + 1), 'Process') }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "cl.exe is not on PATH after $vcvars" }
    if ($env:VSCMD_ARG_TGT_ARCH -ne $Arch) { throw "$vcvars set VSCMD_ARG_TGT_ARCH=$($env:VSCMD_ARG_TGT_ARCH), wanted $Arch" }
    # A calling-convention modifier on the wrong declarator can silently become cdecl
    # on x86. /WX alone is not a gate when a dependency selects /W0 or /WX-.
    # _CL_ is appended after command-line flags and also reaches nested builds.
    # External level 0 suppresses even /we; level 1 admits these two diagnostics.
    if ($Arch -eq 'x86') {
        $policy = (@($env:_CL_, '/external:W1 /we4229 /we4440') | Where-Object { $_ }) -join ' '
        if ($policy.Length -gt 1024) { throw 'x86 compiler policy exceeds the MSVC _CL_ limit' }
        $env:_CL_ = $policy
    }
    return $install
}

# Runs a native program with exact arguments and returns its exit code, stdout and stderr, without the
# PowerShell pipeline re-encoding anything or turning stderr into error records.
function Invoke-Capture([string]$Exe, [string[]]$Arguments = @()) {
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    return [pscustomobject]@{ Code = $p.ExitCode; Out = $out; Err = $err.Result }
}

# SHA-256 of a program's stdout, streamed (git diff of a large tree does not go through a string).
function Get-OutputSha256([string]$Exe, [string[]]$Arguments) {
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe)
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($p.StandardOutput.BaseStream) } finally { $sha.Dispose() }
    $p.WaitForExit()
    $null = $err.Result
    if ($p.ExitCode -ne 0) { return $null }
    return [BitConverter]::ToString($hash).Replace('-', '')
}

# First non-empty line a tool prints for its version switch, or $null if it cannot be run.
function Get-ToolVersion([string]$Exe, [string[]]$Arguments = @('--version')) {
    if (-not $Exe) { return $null }
    try {
        $r = Invoke-Capture $Exe $Arguments
        $line = @(($r.Out + "`n" + $r.Err) -split "`r?`n" | Where-Object { $_.Trim() }) | Select-Object -First 1
        if ($line) { return $line.Trim() } else { return $null }
    } catch { return $null }
}

# cl prints its banner on stderr, localized; the version number is the part that is not.
function Get-ClVersion {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) { return $null }
    $r = Invoke-Capture $cl.Source @()
    if ($r.Err -match '(\d+\.\d+\.\d+(\.\d+)?)') { return $Matches[1] }
    return $null
}

function Resolve-Tool([string]$Explicit, [string]$Name) {
    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw "$Name not found at $Explicit" }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $c = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { return $null }
    return $c.Source
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# What source tree a build came from. A git checkout gives its commit plus a fingerprint of the working-tree
# changes (the lab's Mesa trees carry their patches as uncommitted changes); anything else gives VERSION only.
# --no-optional-locks: identifying a tree must not rewrite its index.
function Get-SourceIdentity([string]$Path) {
    $id = [ordered]@{
        path = $Path; git = $false; commit = $null; describe = $null
        modified_entries = $null; untracked_entries = $null; worktree_diff_sha256 = $null
        version_file = $null; note = $null
    }
    $versionFile = Join-Path $Path 'VERSION'
    if (Test-Path -LiteralPath $versionFile) { $id.version_file = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim() }
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($git -and (Test-Path -LiteralPath (Join-Path $Path '.git'))) {
        $base = @('-C', $Path, '--no-optional-locks')
        $head = Invoke-Capture $git.Source ($base + @('rev-parse', 'HEAD'))
        if ($head.Code -eq 0) {
            $id.git = $true
            $id.commit = $head.Out.Trim()
            $d = Invoke-Capture $git.Source ($base + @('describe', '--tags', '--always'))
            if ($d.Code -eq 0) { $id.describe = $d.Out.Trim() }
            # --ignore-submodules=none: a submodule whose own tree is modified counts as a modified entry here,
            # because the engines carry their sources in submodules and a build from such a tree is a build from
            # no commit. The release gate refuses a record with a modified entry (provenance.py build_records_ok).
            $st = Invoke-Capture $git.Source ($base + @('status', '--porcelain', '--ignore-submodules=none'))
            if ($st.Code -eq 0) {
                $lines = @($st.Out -split "`n" | Where-Object { $_.Trim() })
                $id.untracked_entries = @($lines | Where-Object { $_.StartsWith('??') }).Count
                $id.modified_entries = $lines.Count - $id.untracked_entries
            }
            $id.worktree_diff_sha256 = Get-OutputSha256 $git.Source ($base + @('diff', '--binary', 'HEAD'))
            $id.note = 'worktree_diff_sha256 is the SHA-256 of "git diff --binary HEAD" (tracked files only)'
        }
    }
    if (-not $id.git) { $id.note = 'not a git checkout: identify it by the companion repository branch or the experiment source manifest' }
    return $id
}

# Which revision of the recipe produced a build: the repository commit plus the hashes of the recipe files
# themselves, because the working tree may differ from that commit.
function Get-RecipeIdentity([string]$Repo, [string[]]$Files) {
    $commit = $null
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($git) {
        $r = Invoke-Capture $git.Source @('-C', $Repo, '--no-optional-locks', 'rev-parse', 'HEAD')
        if ($r.Code -eq 0) { $commit = $r.Out.Trim() }
    }
    $hashes = [ordered]@{}
    foreach ($f in $Files) { $hashes[(Split-Path -Leaf $f)] = Get-FileSha256 $f }
    return [ordered]@{ repository_commit = $commit; files_sha256 = $hashes }
}

# The MSVC switches that make a meson build give the same bytes from the same commit in another directory
# (docs/design/reproducible-builds.md). /Brepro: no clock in the objects and the image. /PDBALTPATH:%_PDB%: the
# CodeView record names the PDB file only. /FC with /d1trimfile: __FILE__ and the hash in the name of an anonymous
# namespace start below the source or the build directory, not at the drive. The switches go at the end of an option
# the set already has, because a second -D for the same option replaces the first. The configure gate then compares
# the Build Options line with the merged set.
function Add-ReproducibleMesonOptions([string[]]$Options, [string]$Source, [string]$Build, [ValidateSet('x64', 'x86')][string]$Arch = 'x64') {
    foreach ($dir in $Source, $Build) {
        if ($dir -match '\s') { throw "$dir contains white space: meson splits c_args at white space, so /d1trimfile cannot name it" }
    }
    $compile = "/Brepro /FC /d1trimfile:$Source /d1trimfile:$Build"
    if ($Arch -eq 'x86') { $compile += ' /external:W1 /we4229 /we4440' }
    $add = [ordered]@{ c_args = $compile; cpp_args = $compile; c_link_args = '/Brepro /PDBALTPATH:%_PDB%'; cpp_link_args = '/Brepro /PDBALTPATH:%_PDB%' }
    $merged = [Collections.Generic.List[string]]::new()
    foreach ($o in $Options) { $merged.Add($o) }
    foreach ($name in $add.Keys) {
        $i = 0
        while ($i -lt $merged.Count -and -not $merged[$i].StartsWith("-D$name=")) { $i++ }
        if ($i -lt $merged.Count) { $merged[$i] = "$($merged[$i]) $($add[$name])" } else { $merged.Add("-D$name=$($add[$name])") }
    }
    return @($merged)
}

function Write-Recipe([string]$Path, $Recipe) {
    $json = ($Recipe | ConvertTo-Json -Depth 10) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($Path, $json + "`n", [Text.UTF8Encoding]::new($false))
}

# BD-166: the version resource of a shipped user-mode file is written by the build that produced it, as that build's
# last step, and never by the release: tools\release\build-release.ps1 reads the result back with
# `versionstamp.py check --signed` and rewrites no PE file, because a rewrite there would change a file that
# release-sources.json already pins. Each file this stamps is named under "built" in tools\build\versioninfo.json,
# which is where the release gate learns that it carries its resource from its own build.
#   -Files    the files to stamp, as this build wrote them. Unsigned: a stamp after a signature breaks it.
#   -Names    the name each file carries in the release, in the same order, when a build output is named otherwise
#             (Mesa writes d3d10umd.dll, the release ships bc250d3d.dll). Empty: each file's own name.
#   -Version  the package version, for example 0.7.216.102-tester.31. It has no default: the version belongs to the
#             release, which passes it to every component build it runs (tools\release\build-release.ps1 -Version),
#             and no file of this repository holds it - release-sources.json's kmd_version is the kernel driver's
#             own number, not the package's. Without it nothing is stamped and the build says so, because a host
#             build and a quality gate are not a release; the release itself reads every resource back
#             (versionstamp.py check --signed) and refuses a package whose files carry none.
#   -Flavour  'release' or 'lab': the two flavours of one source line carry different file descriptions.
# The stamp is reproducible: its bytes are a function of the file, the name, the version quad and the flavour, and
# of nothing on this machine or this clock.
#   -OnlyIfShipped  for a build that also produces files no release ships (the probe and test executables of the
#             tool builds): a name that versioninfo.json does not describe is left alone, and the build says so.
#             Without the switch such a name stops the build, which is what a shipped file wants.
function Set-Bc250VersionStamp([string]$Repo, [string[]]$Files, [string[]]$Names = @(), [string]$Version = '',
                               [ValidateSet('release', 'lab')][string]$Flavour = 'release', [string]$Python = '',
                               [switch]$OnlyIfShipped) {
    if (-not $Files -or $Files.Count -eq 0) { return }
    if ($OnlyIfShipped) {
        $table = Get-Content -LiteralPath (Join-Path $Repo 'tools\build\versioninfo.json') -Raw | ConvertFrom-Json
        $keep = @()
        for ($i = 0; $i -lt $Files.Count; $i++) {
            $name = if ($Names.Count) { $Names[$i] } else { Split-Path -Leaf $Files[$i] }
            if ($table.files.PSObject.Properties.Name -contains $name) { $keep += $i }
            else { Write-Host "version stamp: no release ships $name, so it carries no version resource" }
        }
        if ($keep.Count -eq 0) { return }
        $Files = @($keep | ForEach-Object { $Files[$_] })
        if ($Names.Count) { $Names = @($keep | ForEach-Object { $Names[$_] }) }
    }
    if ($Names.Count -and $Names.Count -ne $Files.Count) { throw 'Set-Bc250VersionStamp: one name per file, or none' }
    $tool = Join-Path $Repo 'tools\build\versionstamp.py'
    $inf = Join-Path $Repo 'driver\kmd\bc250kmd.inf'
    foreach ($p in @($tool, $inf)) { if (-not (Test-Path -LiteralPath $p)) { throw "version stamp: no $p in $Repo" } }
    if (-not $Version) {
        Write-Host ('version stamp: no -Version, so ' + (($Files | ForEach-Object { Split-Path -Leaf $_ }) -join ', ') +
                    ' carries no version resource. Only a release build stamps, and the release reads the result ' +
                    'back (versionstamp.py check --signed), which refuses a package of unstamped files.')
        return
    }
    $exe = Resolve-Tool $Python 'python.exe'
    if (-not $exe) { $exe = Resolve-Tool '' 'python' }
    if (-not $exe) { throw 'version stamp: python is not on PATH' }
    for ($i = 0; $i -lt $Files.Count; $i++) {
        $file = (Resolve-Path -LiteralPath $Files[$i]).Path
        $arguments = @($tool, 'stamp', '--version', $Version, '--inf', $inf, '--flavour', $Flavour)
        if ($Names.Count) { $arguments += @('--name', $Names[$i]) }
        Invoke-Checked $exe ($arguments + @($file))
    }
}

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    Write-Host ('> {0} {1}' -f $Exe, ($Arguments -join ' '))
    & $Exe @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Exe) failed with exit code $LASTEXITCODE" }
}
