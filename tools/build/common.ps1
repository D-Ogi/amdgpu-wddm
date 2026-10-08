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
            $st = Invoke-Capture $git.Source ($base + @('status', '--porcelain'))
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
function Add-ReproducibleMesonOptions([string[]]$Options, [string]$Source, [string]$Build) {
    foreach ($dir in $Source, $Build) {
        if ($dir -match '\s') { throw "$dir contains white space: meson splits c_args at white space, so /d1trimfile cannot name it" }
    }
    $compile = "/Brepro /FC /d1trimfile:$Source /d1trimfile:$Build"
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

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    Write-Host ('> {0} {1}' -f $Exe, ($Arguments -join ' '))
    & $Exe @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $Exe) failed with exit code $LASTEXITCODE" }
}
