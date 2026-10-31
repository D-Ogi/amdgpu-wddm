# Pack the llama.cpp asset of an amdgpu-wddm release from a build of build-llama-patched.ps1 -AssetTools.
#
# The result is one zip, llama.cpp-hip-gfx1013-<base>-<release>.zip, that holds one folder of the same name: the
# programs and libraries of the build, build.log, LICENSE and licenses\, patches\, PROVENANCE.txt, README.txt and
# manifest.json. patches\ holds every patch of llama-patches.json in the order they apply, the list itself, and the
# whole difference of the built source copy against the upstream base. manifest.json holds the SHA-256 of every
# other file. The HIP runtime is not in the zip: the programs load it from tools\hip of the installed release.
#
# The script writes no zip unless every check passes:
# - the build log names the build-llama-patched.ps1 of the patch directory with its SHA-256, and holds the SHA-256
#   of the ggml-hip.dll that goes into the zip,
# - the build used -AssetTools and -Bc250AllocationCap, and the build cache agrees with the log about the cap,
# - the patch directory has no change that is not committed, because PROVENANCE.txt names the commit,
# - the source copy carries every marker of llama-patches.json, and differs from the upstream base in exactly the
#   files that the patches of the list change,
# - every program and library of the asset is in the build, and each library that they import is in the zip, in
#   the HIP runtime of the release, in Windows or in the Visual C++ runtime,
# - the installer of the commit accepts no Visual C++ runtime older than the one that the compiler used,
# - no HIP runtime file is in the bin directory of the build or in the zip.
# The script never runs a produced binary. The upstream repository is only read.
param(
    # The build directory of build-llama-patched.ps1 -AssetTools: CMakeCache.txt, build.ninja and bin\.
    [Parameter(Mandatory=$true)][string]$Build,
    # The complete output of that build, with the lines of the builder. It goes into the zip as build.log.
    [Parameter(Mandatory=$true)][string]$BuildLog,
    # A git repository of llama.cpp that holds the base commit of llama-patches.json.
    [Parameter(Mandatory=$true)][string]$UpstreamRepo,
    # The amdgpu-wddm release of this asset, for example 0.7.216.107-tester.35. README.txt names it as the
    # requirement, and the asset name ends with it.
    [Parameter(Mandatory=$true)][string]$Release,
    # A text file with the measurements of these binaries on a BC-250. It becomes the MEASURED section of README.txt,
    # so its first line is the heading of the section and starts with MEASURED.
    [Parameter(Mandatory=$true)][string]$MeasuredFile,
    [Parameter(Mandatory=$true)][string]$OutDir,
    # The source copy that the build compiled. Empty means CMAKE_HOME_DIRECTORY of the build cache. A value must name
    # that same directory.
    [string]$Source = '',
    # The directory of llama-patches.json, the patches, the builder and the README template. Empty means the
    # directory of this script. It must be in a git checkout: PROVENANCE.txt names the commit of that checkout.
    [string]$PatchDir = '',
    # The files of tools\hip in the release. The programs can import these and no other HIP library.
    [string[]]$RuntimeFiles = @('amdhip64.dll', 'bc250hipblas.dll'),
    [string]$RepositoryUrl = 'https://github.com/D-Ogi/amdgpu-wddm',
    # Replace a zip of the same name in the output directory.
    [switch]$Force,
    [string]$Git = 'git'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not $PatchDir) {
    $PatchDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
}
$self = if ($PSCommandPath) { $PSCommandPath } else { $MyInvocation.MyCommand.Path }
$utf8 = New-Object Text.UTF8Encoding $false

function Fail([string]$message) { throw "pack-llama-asset: $message" }
function Get-NormalPath([string]$path) {
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path)
    return [IO.Path]::GetFullPath($full).TrimEnd('\')
}
# git in a directory. The result is the lines that git writes. An exit code outside $Ok stops the script.
function Invoke-Git([string]$Dir, [string[]]$Arguments, [string]$What, [int[]]$Ok = @(0)) {
    $out = @(& $Git -C $Dir @Arguments)
    if ($Ok -notcontains $LASTEXITCODE) { Fail "$What failed: git $($Arguments -join ' ') ended with exit $LASTEXITCODE" }
    return $out
}
# git in the work area, without the user and system configuration and without a repository above the work area, so
# that the difference and its check do not depend on the settings of the computer that packs.
function Invoke-CleanGit([string]$Dir, [string[]]$Arguments, [string]$What, [int[]]$Ok = @(0)) {
    $names = 'HOME', 'XDG_CONFIG_HOME', 'GIT_CONFIG_NOSYSTEM', 'GIT_CEILING_DIRECTORIES', 'GIT_CONFIG_PARAMETERS',
        'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE'
    $saved = @{}
    foreach ($n in $names) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
        foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $null) }
        $env:HOME = $cleanHome
        $env:XDG_CONFIG_HOME = $cleanHome
        $env:GIT_CONFIG_NOSYSTEM = '1'
        $env:GIT_CEILING_DIRECTORIES = Split-Path -Parent $work
        return Invoke-Git $Dir $Arguments $What $Ok
    } finally {
        foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    }
}
# The blob names of files, relative to $Dir, without a filter: the bytes as they are. The paths go on the command
# line in groups, because Windows PowerShell puts a byte order mark before the standard input of a native command
# on a UTF-8 console, and git then reads the first path with that mark.
function Get-BlobNames([string]$Dir, [string[]]$Paths) {
    $names = New-Object 'System.Collections.Generic.List[string]'
    $i = 0
    while ($i -lt $Paths.Count) {
        $group = New-Object 'System.Collections.Generic.List[string]'
        $length = 0
        while ($i -lt $Paths.Count -and ($group.Count -eq 0 -or $length + $Paths[$i].Length -lt 8000)) {
            $group.Add($Paths[$i])
            $length += $Paths[$i].Length + 3
            $i++
        }
        $out = @(& $Git -C $Dir hash-object --no-filters -- $group.ToArray())
        if ($LASTEXITCODE -or $out.Count -ne $group.Count) { Fail "git hash-object failed in $Dir" }
        $names.AddRange([string[]]$out)
    }
    return $names.ToArray()
}
function Get-Sha256([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('X2') }) -join '') } finally { $sha.Dispose() }
}
# The text between the comment delimiters around the one line of $Path that holds $Marker, without the
# indentation that all its lines share. This is how the licence of a single-header library is taken out of it.
function Get-CommentBlock([string]$Path, [string]$Marker) {
    $lines = [IO.File]::ReadAllText($Path).Replace("`r`n", "`n").Split("`n")
    $at = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Contains($Marker)) { $i } })
    if ($at.Count -ne 1) { Fail "$Path holds the licence marker '$Marker' $($at.Count) times, not once" }
    $first = $at[0]; while ($first -ge 0 -and $lines[$first].Trim() -ne '/*') { $first-- }
    $last = $at[0]; while ($last -lt $lines.Count -and $lines[$last].Trim() -ne '*/') { $last++ }
    if ($first -lt 0 -or $last -ge $lines.Count) { Fail "no comment delimiters around '$Marker' in $Path" }
    $block = @($lines[($first + 1)..($last - 1)] | ForEach-Object { $_.TrimEnd() })
    while ($block.Count -and -not $block[0]) { $block = @($block | Select-Object -Skip 1) }
    while ($block.Count -and -not $block[-1]) { $block = @($block | Select-Object -SkipLast 1) }
    $indent = @($block | Where-Object { $_ } | ForEach-Object { $_.Length - $_.TrimStart().Length } | Measure-Object -Minimum).Minimum
    return (($block | ForEach-Object { if ($_.Length -ge $indent) { $_.Substring($indent) } else { $_ } }) -join "`r`n") + "`r`n"
}
# The names of the libraries that a PE32+ image imports, directly and delay-loaded.
function Get-PeImports([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    function U16([long]$o) { if ($o -lt 0 -or $o + 2 -gt $b.Length) { Fail "$Path is cut short at $o" }; [BitConverter]::ToUInt16($b, $o) }
    function U32([long]$o) { if ($o -lt 0 -or $o + 4 -gt $b.Length) { Fail "$Path is cut short at $o" }; [BitConverter]::ToUInt32($b, $o) }
    if ($b.Length -lt 64 -or (U16 0) -ne 0x5A4D) { Fail "$Path is not a PE image" }
    $pe = [long](U32 0x3C)
    if ((U32 $pe) -ne 0x4550) { Fail "$Path is not a PE image" }
    if ((U16 ($pe + 4)) -ne 0x8664) { Fail "$Path is not an x64 image" }
    $sections = U16 ($pe + 6)
    $opt = $pe + 24
    if ((U16 $opt) -ne 0x20B) { Fail "$Path is not a PE32+ image" }
    $dirCount = U32 ($opt + 108)
    $dd = $opt + 112
    $table = $opt + (U16 ($pe + 20))
    function Get-Offset([long]$rva) {
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $table + 40 * $i
            $va = U32 ($s + 12)
            $span = [Math]::Max([long](U32 ($s + 8)), [long](U32 ($s + 16)))
            if ($rva -ge $va -and $rva -lt $va + $span) { return $rva - $va + (U32 ($s + 20)) }
        }
        Fail "$Path has the address $rva in no section"
    }
    function Get-Name([long]$rva) {
        $o = Get-Offset $rva
        $e = $o
        while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }
        return [Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    }
    $names = @()
    if ($dirCount -gt 1 -and (U32 ($dd + 8))) {
        $o = Get-Offset (U32 ($dd + 8))
        for ($n = 0; $n -lt 4096; $n++) {
            if (-not (U32 ($o + 12)) -and -not (U32 ($o + 16))) { break }
            $names += Get-Name (U32 ($o + 12))
            $o += 20
        }
    }
    if ($dirCount -gt 13 -and (U32 ($dd + 104))) {
        $o = Get-Offset (U32 ($dd + 104))
        for ($n = 0; $n -lt 4096; $n++) {
            if (-not (U32 ($o + 4))) { break }
            $names += Get-Name (U32 ($o + 4))
            $o += 32
        }
    }
    return $names
}
# A HIP runtime file, by its name: the runtime and math libraries, a code object, the sample of tools\hip.
function Test-RuntimeName([string]$name) {
    return ($name -match '^(amdhip64|amd_comgr|hiprtc|hipblas|hipblaslt|hipsparse|hipfft|hiprand|hipsolver|rocblas|rocsolver|rocsparse|rocfft|rocrand|miopen|hsa-runtime|bc250hipblas|bc250hsa)') -or
        ($name -match '\.(hsaco|co)$') -or ($name -ieq 'vadd.exe')
}
function ConvertTo-JsonText([string]$s) {
    if ($s -match '[\x00-\x1f]') { Fail "a manifest value holds a control character: $s" }
    return '"' + $s.Replace('\', '\\').Replace('"', '\"') + '"'
}

# ---- the inputs ----------------------------------------------------------------------------------------------------
$Build = Get-NormalPath $Build
$BuildLog = Get-NormalPath $BuildLog
$UpstreamRepo = Get-NormalPath $UpstreamRepo
$MeasuredFile = Get-NormalPath $MeasuredFile
$OutDir = Get-NormalPath $OutDir
$PatchDir = Get-NormalPath $PatchDir
foreach ($p in $Build, $BuildLog, $UpstreamRepo, $MeasuredFile, $PatchDir) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "$p does not exist" }
}
if ($Release -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') { Fail "the release name '$Release' holds a character that the asset name cannot hold" }
$patchList = Get-Content -LiteralPath (Join-Path $PatchDir 'llama-patches.json') -Raw | ConvertFrom-Json
$base = [string]$patchList.base
if ($base -notmatch '^[0-9a-f]{40}$') { Fail 'llama-patches.json names no base commit' }
$short = $base.Substring(0, 8)
$patchSet = @($patchList.patches | ForEach-Object {
    [pscustomobject]@{ Patch = [string]$_.patch; File = [string]$_.file; Marker = [string]$_.marker; What = [string]$_.what }
})
if (-not $patchSet.Count) { Fail 'llama-patches.json names no patch' }
$name = "llama.cpp-hip-gfx1013-$short-$Release"
$zipPath = Join-Path $OutDir "$name.zip"
if ((Test-Path -LiteralPath $zipPath) -and -not $Force) { Fail "$zipPath exists. Pass -Force to replace it." }

# The commit that PROVENANCE.txt names holds every file that this script reads from the patch directory.
$commit = [string]@(Invoke-Git $PatchDir @('rev-parse', '--verify', 'HEAD^{commit}') 'reading the commit of the patch directory')[0]
$dirty = @(Invoke-Git $PatchDir @('status', '--porcelain', '--untracked-files=all', '--', '.') 'git status of the patch directory')
if ($dirty.Count) { Fail "the patch directory has changes that are not committed ($($dirty -join ', ')). PROVENANCE.txt names the commit, so commit them first." }
$commitTime = [long]@(Invoke-Git $PatchDir @('show', '-s', '--format=%ct', $commit) 'reading the commit time')[0]

# ---- the build log and the build cache -----------------------------------------------------------------------------
$builder = Join-Path $PatchDir 'build-llama-patched.ps1'
if (-not (Test-Path -LiteralPath $builder)) { Fail "no builder $builder" }
$builderHash = (Get-FileHash -LiteralPath $builder -Algorithm SHA256).Hash
$logLines = [IO.File]::ReadAllLines($BuildLog)
$builderLines = @($logLines -match '^builder [0-9A-F]{64} ')
if ($builderLines.Count -ne 1) { Fail "the build log holds $($builderLines.Count) builder lines, not one" }
$loggedBuilder = $builderLines[0].Substring(8, 64)
if ($loggedBuilder -ne $builderHash) { Fail "the build log names the builder $loggedBuilder, and $builder is $builderHash. Pack from the checkout that made the build." }
$optionLines = @($logLines -match '^builder options: ')
if ($optionLines.Count -ne 1) { Fail "the build log holds $($optionLines.Count) builder options lines, not one" }
$builderOptions = $optionLines[0].Substring('builder options: '.Length).Trim()
if (" $builderOptions " -notmatch ' -AssetTools ') { Fail 'the build was made without -AssetTools, so it has no llama-server or llama-cli' }
if (" $builderOptions " -match ' -Bc250AllocationCap -Bc250AllocationCapBytes (\d+) ') { $cap = [UInt64]$Matches[1] }
else { Fail 'the build was made without -Bc250AllocationCap. The runtime refuses an allocation above its limit, so the asset needs the cap.' }
$compilers = [ordered]@{}
foreach ($line in @($logLines -match '^-- The (C|CXX) compiler identification is ')) {
    if ($line -match '^-- The (C|CXX) compiler identification is (.+)$') { $compilers[$(if ($Matches[1] -eq 'C') { 'C' } else { 'C++' })] = $Matches[2].Trim() }
}
if ($compilers.Count -ne 2) { Fail 'the build log does not name the C and the C++ compiler' }
$compilerText = if (@($compilers.Values | Select-Object -Unique).Count -eq 1) { "$(@($compilers.Values)[0]) for C and C++" }
    else { (@($compilers.Keys | ForEach-Object { "$($compilers[$_]) for $_" })) -join ', ' }

$binDir = Join-Path $Build 'bin'
$hipHash = (Get-FileHash -LiteralPath (Join-Path $binDir 'ggml-hip.dll') -Algorithm SHA256).Hash
if (-not @($logLines -match $hipHash).Count) { Fail "the build log does not hold the SHA-256 $hipHash of bin\ggml-hip.dll, so it is not the log of this build" }

$cache = @{}
foreach ($line in [IO.File]::ReadAllLines((Join-Path $Build 'CMakeCache.txt'))) {
    if ($line -match '^([A-Za-z0-9_.+-]+):([A-Z]+)=(.*)$') { $cache[$Matches[1]] = $Matches[3] }
}
foreach ($want in 'GGML_HIP=ON', 'GGML_CUDA_FA=ON', 'BUILD_SHARED_LIBS=ON', 'LLAMA_BUILD_SERVER=ON') {
    $k, $v = $want -split '=', 2
    if ([string]$cache[$k] -ne $v) { Fail "the build cache has $k=$($cache[$k]), not $v" }
}
$capDefine = "-DBC250_HIP_MAX_ALLOC_BYTES=0x$($cap.ToString('x'))ULL"
if (-not ([string]$cache['CMAKE_CXX_FLAGS']).Contains($capDefine)) { Fail "CMAKE_CXX_FLAGS of the build cache does not hold $capDefine, which the builder options name" }
# The Visual C++ runtime that the programs need: at least the version of the MSVC headers and libraries that the
# compiler used. Clang gives that version as the MSVC version it simulates, 19.N for the runtime 14.N. README.txt
# sends the user to the installer of the release for this runtime, so that installer must accept no older one.
$cmakeVersion = "$($cache['CMAKE_CACHE_MAJOR_VERSION']).$($cache['CMAKE_CACHE_MINOR_VERSION']).$($cache['CMAKE_CACHE_PATCH_VERSION'])"
$cxxCompilerFile = Join-Path $Build "CMakeFiles\$cmakeVersion\CMakeCXXCompiler.cmake"
if (-not (Test-Path -LiteralPath $cxxCompilerFile)) { Fail "the build has no $cxxCompilerFile" }
$cxxCompiler = [IO.File]::ReadAllText($cxxCompilerFile)
if ($cxxCompiler -notmatch 'set\(CMAKE_CXX_SIMULATE_ID "MSVC"\)') { Fail "$cxxCompilerFile does not name the MSVC that the C++ compiler uses" }
if ($cxxCompiler -notmatch 'set\(CMAKE_CXX_SIMULATE_VERSION "19\.(\d+)"\)') { Fail "$cxxCompilerFile does not name an MSVC 19.N version" }
$vcNeeded = [int]$Matches[1]
$installerScript = @(Invoke-Git $PatchDir @('show', "$($commit):tools/release/installer/common.ps1") 'reading the installer of the commit')
$vcMajor = @($installerScript -match '^\$script:VcRuntimeMinMajor = \d+\s*$')
$vcMinor = @($installerScript -match '^\$script:VcRuntimeMinMinor = \d+\s*$')
if ($vcMajor.Count -ne 1 -or $vcMinor.Count -ne 1) { Fail 'tools/release/installer/common.ps1 of the commit does not name the lowest Visual C++ runtime that it accepts' }
$vcAccepted = "$([int]($vcMajor[0] -replace '\D', '')).$([int]($vcMinor[0] -replace '\D', ''))"
if ($vcAccepted -notlike '14.*' -or [int]$vcAccepted.Split('.')[1] -lt $vcNeeded) {
    Fail "the build needs the Visual C++ runtime 14.$vcNeeded or newer, and the installer of the commit accepts $vcAccepted"
}
$cmakeHome = [string]$cache['CMAKE_HOME_DIRECTORY']
if (-not $cmakeHome) { Fail 'the build cache names no source directory (CMAKE_HOME_DIRECTORY)' }
if (-not $Source) { $Source = $cmakeHome }
elseif ((Get-NormalPath $Source) -ine (Get-NormalPath $cmakeHome)) { Fail "the build compiled $cmakeHome, not $Source" }
$Source = Get-NormalPath $Source
if ($Build.StartsWith("$Source\", [StringComparison]::OrdinalIgnoreCase)) { Fail 'the build directory is inside the source copy' }
$rocmRoot = [string]$cache['CMAKE_PREFIX_PATH']

# The compile line of the HIP backend, for PROVENANCE.txt, without the local paths.
$hipFlags = $null
$ninja = [IO.File]::ReadAllLines((Join-Path $Build 'build.ninja'))
for ($i = 0; $i -lt $ninja.Count -and -not $hipFlags; $i++) {
    if ($ninja[$i] -notlike 'build ggml/src/ggml-hip/CMakeFiles/ggml-hip.dir/*/ggml-cuda.cu.obj:*') { continue }
    for ($j = $i + 1; $j -lt $ninja.Count -and $ninja[$j] -match '^\s'; $j++) {
        if ($ninja[$j] -match '^\s+FLAGS = (.*)$') { $hipFlags = $Matches[1]; break }
    }
}
if (-not $hipFlags) { Fail 'build.ninja has no compile rule for ggml-cuda.cu of ggml-hip' }
if ($hipFlags -notmatch '--offload-arch=gfx1013(\s|$)') { Fail "ggml-cuda.cu is not compiled for gfx1013: $hipFlags" }
foreach ($pair in @(@($Source, '<source>'), @($Build, '<build>'), @($rocmRoot, '<rocm-root>'))) {
    if (-not $pair[0]) { continue }
    foreach ($form in @($pair[0], $pair[0].Replace('\', '/'), $pair[0].Replace('/', '\'))) {
        $hipFlags = [regex]::Replace($hipFlags, [regex]::Escape($form), $pair[1], 'IgnoreCase')
    }
}
$hipFlags = ($hipFlags -replace '\s+', ' ').Trim()

# ---- the programs and libraries ------------------------------------------------------------------------------------
$programs = 'llama-server', 'llama-cli', 'llama-completion', 'llama-bench'
$binaries = @('ggml.dll', 'ggml-base.dll', 'ggml-cpu.dll', 'ggml-hip.dll', 'llama.dll', 'llama-common.dll', 'mtmd.dll') +
    @($programs | ForEach-Object { "$_.exe"; "$_-impl.dll" })
$foreign = @(Get-ChildItem -LiteralPath $binDir -Recurse -File -Force | Where-Object { Test-RuntimeName $_.Name } | ForEach-Object Name)
if ($foreign.Count) { Fail "the bin directory of the build holds HIP runtime files ($($foreign -join ', ')). The programs take the runtime of the installed release. Remove these files from the build and pack again." }
foreach ($file in $binaries) { if (-not (Test-Path -LiteralPath (Join-Path $binDir $file))) { Fail "the build has no bin\$file" } }
# Every library that the programs load is in the zip, in the runtime of the release, or in Windows. The Visual C++
# runtime is part of the requirements (the amdgpu-wddm installer installs it).
$vcRuntime = 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll', 'vcomp140.dll'
$runtimeUsed = @()
foreach ($file in $binaries) {
    foreach ($dll in (Get-PeImports (Join-Path $binDir $file))) {
        if ($binaries -contains $dll) { continue }
        if (Test-RuntimeName $dll) {
            if ($RuntimeFiles -notcontains $dll) { Fail "$file imports $dll, and tools\hip of the release holds only $($RuntimeFiles -join ', ')" }
            $runtimeUsed += @($RuntimeFiles | Where-Object { $_ -ieq $dll })
            continue
        }
        if ($vcRuntime -contains $dll -or $dll -match '^(api|ext)-ms-win-' -or
            (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $dll))) { continue }
        Fail "$file imports $dll, which is not in the zip, not in the HIP runtime and not part of Windows or of the Visual C++ runtime"
    }
}
$runtimeUsed = @($RuntimeFiles | Where-Object { $runtimeUsed -contains $_ })
if (-not $runtimeUsed.Count) { Fail 'no program or library imports the HIP runtime' }

# ---- the source copy against the upstream base ---------------------------------------------------------------------
foreach ($item in $patchSet) {
    $path = Join-Path $Source $item.File
    if (-not (Test-Path -LiteralPath $path)) { Fail "the source copy has no $($item.File), which $($item.Patch) changes" }
    if (-not [IO.File]::ReadAllText($path).Contains($item.Marker)) { Fail "the source copy lacks the marker of $($item.Patch) ($($item.What)) in $($item.File)" }
}
# The files that the patches change, create or delete, from their ---/+++ lines.
$touched = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
foreach ($item in $patchSet) {
    $patchPath = Join-Path $PatchDir $item.Patch
    if (-not (Test-Path -LiteralPath $patchPath)) { Fail "llama-patches.json names $($item.Patch), which is not in $PatchDir" }
    $lines = [IO.File]::ReadAllLines($patchPath)
    for ($i = 0; $i + 1 -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^--- (\S+)') { continue }
        $from = $Matches[1]
        if ($lines[$i + 1] -notmatch '^\+\+\+ (\S+)') { continue }
        $path = if ($Matches[1] -ne '/dev/null') { $Matches[1] } else { $from }
        if ($path -match '^[ab]/(.+)$') { [void]$touched.Add($Matches[1]) }
        else { Fail "$($item.Patch) names the file $path without the a/ or b/ prefix" }
    }
}
$resolved = [string]@(Invoke-Git $UpstreamRepo @('rev-parse', '--verify', '-q', "$base^{commit}") "finding $base in $UpstreamRepo" @(0, 1))[0]
if ($resolved -ne $base) { Fail "$UpstreamRepo does not hold the base commit $base" }
$tree = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::Ordinal)
foreach ($line in (Invoke-Git $UpstreamRepo @('ls-tree', '-r', '--full-tree', $base) 'listing the base tree')) {
    if ($line -notmatch '^(\d{6}) (\w+) ([0-9a-f]{40})\t(.+)$') { Fail "unexpected ls-tree line: $line" }
    if ($Matches[2] -ne 'blob') { continue }
    if ($Matches[4].StartsWith('"')) { Fail "the base holds a path that git quotes: $($Matches[4])" }
    $tree[$Matches[4]] = $Matches[3]
}
$srcFiles = New-Object 'System.Collections.Generic.List[string]'
foreach ($f in [IO.Directory]::EnumerateFiles($Source, '*', [IO.SearchOption]::AllDirectories)) {
    $rel = $f.Substring($Source.Length + 1).Replace('\', '/')
    if ($rel -eq '.git' -or $rel.StartsWith('.git/')) { continue }
    if ($rel -match '[^\x20-\x7e]|"') { Fail "the source copy holds a file name that is not plain ASCII: $rel" }
    $srcFiles.Add($rel)
}
$srcBlobs = @(Get-BlobNames $Source $srcFiles.ToArray())
$changed = @(); $added = @(); $removed = @()
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
for ($i = 0; $i -lt $srcFiles.Count; $i++) {
    $rel = $srcFiles[$i]
    [void]$seen.Add($rel)
    if (-not $tree.ContainsKey($rel)) { $added += $rel }
    elseif ($tree[$rel] -ne $srcBlobs[$i]) { $changed += $rel }
}
foreach ($rel in $tree.Keys) { if (-not $seen.Contains($rel)) { $removed += $rel } }
$differ = @($changed) + @($added) + @($removed)
$unlisted = @($differ | Where-Object { -not $touched.Contains($_) } | Sort-Object)
if ($unlisted.Count) { Fail "the source copy differs from $short in $($unlisted.Count) file(s) that no patch of llama-patches.json changes: $(($unlisted | Select-Object -First 10) -join ', ')" }
$same = @($touched | Where-Object { $differ -notcontains $_ } | Sort-Object)
if ($same.Count) { Fail "the patches change $(($same | Select-Object -First 10) -join ', '), and the source copy holds the base version" }

$work = Join-Path $OutDir ('.pack-' + [guid]::NewGuid().ToString('N'))
$cleanHome = Join-Path $work 'home'
$partial = "$zipPath.partial"
try {
    $dirA = Join-Path $work 'a'
    $dirB = Join-Path $work 'b'
    $dirC = Join-Path $work 'c'
    foreach ($d in $dirA, $dirB, $dirC, $cleanHome) { [void][IO.Directory]::CreateDirectory($d) }
    # a holds the base version of every changed or deleted file, b the built version of every changed or new file.
    $fromBase = @($changed) + @($removed)
    if ($fromBase.Count) {
        $tar = Join-Path $work 'base.tar'
        Invoke-Git $UpstreamRepo (@('-c', 'core.autocrlf=false', '--literal-pathspecs', 'archive', '--format=tar', '-o', $tar, $base, '--') + $fromBase) 'git archive of the base files' | Out-Null
        & (Join-Path ([Environment]::SystemDirectory) 'tar.exe') -xf $tar -C $dirA
        if ($LASTEXITCODE) { Fail 'tar could not extract the base files' }
        $baseBlobs = @(Get-BlobNames $dirA $fromBase)
        for ($i = 0; $i -lt $fromBase.Count; $i++) {
            if ($baseBlobs[$i] -ne $tree[$fromBase[$i]]) { Fail "the base copy of $($fromBase[$i]) is not the blob of $short" }
        }
        foreach ($rel in $fromBase) {
            $to = Join-Path $dirC $rel
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $to))
            [IO.File]::Copy((Join-Path $dirA $rel), $to)
        }
    }
    foreach ($rel in @($changed) + @($added)) {
        $to = Join-Path $dirB $rel
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $to))
        [IO.File]::Copy((Join-Path $Source $rel), $to)
    }
    $diffName = "source-vs-upstream-$short.diff"
    $diffPath = Join-Path $work $diffName
    Invoke-CleanGit $work @('diff', '--no-index', '--no-prefix', '--binary', '--full-index', '--no-color', '--no-ext-diff',
        '--no-textconv', '--no-renames', '--diff-algorithm=myers', '-U3', '--inter-hunk-context=0', "--output=$diffPath", 'a', 'b') 'git diff of the base and the source copy' @(1) | Out-Null
    # git diff --no-index names a file that only one side has with that side's directory twice ("diff --git a/x a/x"
    # for a deleted file). The header line gets the usual a/ and b/ form, and the bytes of every other line stay.
    $raw = [IO.File]::ReadAllBytes($diffPath)
    $fixed = New-Object IO.MemoryStream
    $start = 0
    while ($start -lt $raw.Length) {
        $end = [Array]::IndexOf($raw, [byte]10, $start)
        if ($end -lt 0) { $end = $raw.Length - 1 }
        $count = $end - $start + 1
        $line = [Text.Encoding]::ASCII.GetString($raw, $start, [Math]::Min($count, 4096))
        if ($line -match '^diff --git ([ab])/(\S+) \1/\2\n$') {
            $bytes = [Text.Encoding]::ASCII.GetBytes("diff --git a/$($Matches[2]) b/$($Matches[2])`n")
            $fixed.Write($bytes, 0, $bytes.Length)
        } else { $fixed.Write($raw, $start, $count) }
        $start = $end + 1
    }
    [IO.File]::WriteAllBytes($diffPath, $fixed.ToArray())
    # The difference turns the base files into the built source files, byte for byte.
    Invoke-CleanGit $dirC @('apply', '-p1', '--whitespace=nowarn', $diffPath) 'git apply of the difference on the base files' | Out-Null
    $after = @([IO.Directory]::EnumerateFiles($dirC, '*', [IO.SearchOption]::AllDirectories) | ForEach-Object { $_.Substring($dirC.Length + 1).Replace('\', '/') } | Sort-Object)
    $want = @(@($changed) + @($added) | Sort-Object)
    if (($after -join '|') -cne ($want -join '|')) { Fail 'the difference applied on the base files does not give the files of the source copy' }
    $afterBlobs = @(Get-BlobNames $dirC $after)
    $wantBlobs = @(Get-BlobNames $Source $after)
    for ($i = 0; $i -lt $after.Count; $i++) {
        if ($afterBlobs[$i] -ne $wantBlobs[$i]) { Fail "the difference applied on the base gives another $($after[$i]) than the source copy" }
    }

    # ---- the files of the folder -----------------------------------------------------------------------------------
    # An entry has its path in the folder and either the file it copies or the bytes it writes.
    $entries = New-Object 'System.Collections.Generic.List[object]'
    function Add-File([string]$path, [string]$from) { $entries.Add([pscustomobject]@{ Path = $path; File = $from; Bytes = $null }) }
    function Add-Text([string]$path, [string]$text) { $entries.Add([pscustomobject]@{ Path = $path; File = $null; Bytes = $utf8.GetBytes($text) }) }
    foreach ($file in $binaries) { Add-File $file (Join-Path $binDir $file) }
    Add-File 'build.log' $BuildLog
    # The licence of llama.cpp, the licences that llama.cpp keeps in licenses\, and the licences of the libraries in
    # vendor\ that the programs compile in. A library that this table does not know stops the script, so that its
    # licence cannot go missing.
    Add-File 'LICENSE' (Join-Path $Source 'LICENSE')
    $licenceDir = Join-Path $Source 'licenses'
    if (Test-Path -LiteralPath $licenceDir) { foreach ($f in Get-ChildItem -LiteralPath $licenceDir -File) { Add-File "licenses/$($f.Name)" $f.FullName } }
    $vendored = @{
        'cpp-httplib' = @(@{ Entry = 'LICENSE-httplib'; From = 'vendor/cpp-httplib/LICENSE' })
        'hash' = @(@{ Entry = 'LICENSE-xxhash'; From = 'vendor/hash/xxhash/LICENSE' },
            @{ Entry = 'LICENSE-rotate-bits'; From = 'vendor/hash/rotate-bits/LICENSE.md' },
            @{ Entry = 'LICENSE-sha256'; From = 'vendor/hash/sha256/LICENSE' })
        'miniaudio' = @(@{ Entry = 'LICENSE-miniaudio'; From = 'vendor/miniaudio/miniaudio.h'; Marker = 'This software is available as a choice of the following licenses.' })
        'nlohmann' = @()
        'sheredom' = @(@{ Entry = 'LICENSE-subprocess'; From = 'vendor/sheredom/subprocess.h'; Marker = 'This is free and unencumbered software released into the public domain.' })
        'stb' = @(@{ Entry = 'LICENSE-stb_image'; From = 'vendor/stb/stb_image.h'; Marker = 'This software is available under 2 licenses' })
    }
    foreach ($d in Get-ChildItem -LiteralPath (Join-Path $Source 'vendor') -Directory) {
        if (-not $vendored.ContainsKey($d.Name)) { Fail "the source copy vendors $($d.Name), and this script has no licence entry for it" }
    }
    if (@($entries | Where-Object { $_.Path -eq 'licenses/LICENSE-jsonhpp' }).Count -ne 1) { Fail 'the source copy has no licenses/LICENSE-jsonhpp for vendor/nlohmann' }
    foreach ($key in @($vendored.Keys | Sort-Object)) {
        foreach ($l in $vendored[$key]) {
            $from = Join-Path $Source $l.From
            if (-not (Test-Path -LiteralPath $from)) { Fail "the source copy has no $($l.From)" }
            if ($l.Marker) { Add-Text "licenses/$($l.Entry)" (Get-CommentBlock $from $l.Marker) } else { Add-File "licenses/$($l.Entry)" $from }
        }
    }
    foreach ($item in $patchSet) { Add-File "patches/$($item.Patch)" (Join-Path $PatchDir $item.Patch) }
    Add-File 'patches/llama-patches.json' (Join-Path $PatchDir 'llama-patches.json')
    Add-File "patches/$diffName" $diffPath

    # PROVENANCE.txt: the upstream revision, one line for each local change with its author, and how the build was made.
    $p = New-Object 'System.Collections.Generic.List[string]'
    $p.Add("PROVENANCE: llama.cpp https://github.com/ggml-org/llama.cpp, MIT (LICENSE), upstream revision $base.")
    $p.Add('')
    $p.Add('Local changes to that revision, in the order they apply. patches/llama-patches.json is the same list, with')
    $p.Add("the string that shows each change in the source. patches/$diffName is the whole")
    $p.Add('difference of the built source copy against the upstream revision: git apply -p1 of it in a copy of that')
    $p.Add('revision gives the built source.')
    $n = 0
    foreach ($item in $patchSet) {
        $n++
        $text = [IO.File]::ReadAllText((Join-Path $PatchDir $item.Patch))
        $by = ''
        if ($text -match '(?m)^From: (.+?)\s*(<[^>]*>)?\s*$') {
            # A patch in mail form keeps the author of the change, and its note names where it comes from.
            $by = " By $($Matches[1].Trim())"
            $note = Join-Path $PatchDir ([IO.Path]::ChangeExtension($item.Patch, '.md'))
            if (-not (Test-Path -LiteralPath $note)) { $note = Join-Path $PatchDir ([IO.Path]::ChangeExtension($item.Patch, '.PROVENANCE.md')) }
            if (Test-Path -LiteralPath $note) {
                $line = @(Get-Content -LiteralPath $note | Where-Object { $_ -like 'PROVENANCE:*' }) | Select-Object -First 1
                if ($line -match 'from (https?://[^\s,]+?)[.,]?(\s|$)') { $by += ", from $($Matches[1])" }
            }
        } else {
            # A patch without a mail header was written in this repository: its author is the author of the commit
            # that added the patch file.
            $authors = @(Invoke-Git $PatchDir @('log', '--format=%an', '--diff-filter=A', '--', $item.Patch) "reading the author of $($item.Patch)")
            if ($authors.Count) { $by = " By $($authors[$authors.Count - 1])" }
        }
        if ($by) { $by += '.' }
        $p.Add(('{0,2}. patches/{1}: {2} ({3}).{4}' -f $n, $item.Patch, $item.What, $item.File, $by))
    }
    $p.Add('')
    $p.Add("Build: $RepositoryUrl commit $commit")
    $p.Add("  compute/hip/patches/build-llama-patched.ps1 $builderOptions")
    $p.Add("  (SHA-256 $builderHash)")
    $p.Add("  compute/hip/patches/pack-llama-asset.ps1 made this folder (SHA-256 $((Get-FileHash -LiteralPath $self -Algorithm SHA256).Hash)).")
    # The compressor of the .NET that runs this script gives the bytes of the zip, so the line names it.
    $runtimeText = try { [Runtime.InteropServices.RuntimeInformation]::FrameworkDescription.Trim() } catch { ".NET $([Environment]::Version)" }
    $p.Add("  It ran in PowerShell $($PSVersionTable.PSVersion) on $runtimeText. The same inputs give the same zip there.")
    $p.Add("Compiler: $compilerText. The C++ compiler also compiles the HIP sources (-x hip).")
    $p.Add("It uses the headers and libraries of MSVC 19.$vcNeeded, so the programs need the Visual C++ runtime 14.$vcNeeded or newer.")
    $p.Add("ggml-cuda.cu flags: $hipFlags")
    $p.Add('build.log holds the complete CMake command line of this build, on the line that starts with "cmake configure:".')
    $keys = 'CMAKE_BUILD_TYPE', 'BUILD_SHARED_LIBS', 'GGML_HIP', 'GGML_CUDA_FA', 'GGML_CUDA_FORCE_MMQ', 'GGML_LTO', 'GGML_NATIVE',
        'GGML_AVX', 'GGML_AVX2', 'GGML_FMA', 'GGML_F16C', 'GGML_BMI2', 'GGML_AVX512', 'LLAMA_BUILD_UI', 'LLAMA_USE_PREBUILT_UI'
    $p.Add("CMake: $(($keys | ForEach-Object { if ($cache.ContainsKey($_)) { "$_=$($cache[$_])" } else { "$_ not set" } }) -join ' ')")
    $p.Add('The web UI is not built and not downloaded. build.log is the complete log of this build.')
    $p.Add("ggml-hip.dll SHA-256 $hipHash.")
    $p.Add('')
    $p.Add('Third-party code inside the binaries: nlohmann/json (MIT, licenses/LICENSE-jsonhpp), cpp-httplib (MIT,')
    $p.Add('licenses/LICENSE-httplib), xxHash (BSD 2-Clause, licenses/LICENSE-xxhash), rotate-bits (MIT,')
    $p.Add('licenses/LICENSE-rotate-bits), the SHA-256 code of Igor Pavlov (public domain, licenses/LICENSE-sha256), the')
    $p.Add('SHA-1 code of Steve Reid (public domain), stb_image (MIT or public domain, licenses/LICENSE-stb_image), miniaudio')
    $p.Add('(public domain or MIT No Attribution, licenses/LICENSE-miniaudio), subprocess.h (public domain,')
    $p.Add('licenses/LICENSE-subprocess).')
    $p.Add("The HIP runtime ($($runtimeUsed -join ', ')) is not in this folder. It comes from tools\hip of amdgpu-wddm $Release.")
    Add-Text 'PROVENANCE.txt' (($p -join "`r`n") + "`r`n")

    # README.txt from the template of the patch directory, with the release, the runtime, the cap and the measurements.
    $template = Join-Path $PatchDir 'llama-asset-README.txt'
    if (-not (Test-Path -LiteralPath $template)) { Fail "no README template $template" }
    $readme = [IO.File]::ReadAllText($template).Replace("`r`n", "`n")
    $measured = [IO.File]::ReadAllText($MeasuredFile).Replace("`r`n", "`n").Trim("`n", ' ', "`t")
    if ($measured -notmatch '^MEASURED') { Fail "the first line of $MeasuredFile is not a MEASURED heading" }
    $gib = [UInt64][Math]::Ceiling($cap / 1GB)
    $gap = $gib * 1GB - $cap
    $capText = if ($gap -eq 0) { "$gib GiB" } elseif ($gap % 1KB -eq 0) { "$gib GiB minus $($gap / 1KB) KiB" } else { "$cap bytes" }
    $values = [ordered]@{
        '{{NAME}}' = $name; '{{RELEASE}}' = $Release; '{{RUNTIME}}' = ($runtimeUsed -join ' and ')
        '{{CAP_HEX}}' = "0x$($cap.ToString('X'))"; '{{CAP_TEXT}}' = $capText; '{{VC_RUNTIME}}' = "14.$vcNeeded"
        '{{MEASURED}}' = $measured
    }
    foreach ($k in $values.Keys) {
        if (-not $readme.Contains($k)) { Fail "the README template has no $k" }
        $readme = $readme.Replace($k, [string]$values[$k])
    }
    if ($readme -match '\{\{[A-Z_]+\}\}') { Fail "the README template holds the unknown placeholder $($Matches[0])" }
    Add-Text 'README.txt' (($readme.TrimEnd("`n") -replace "`n", "`r`n") + "`r`n")

    # manifest.json: the SHA-256 of every other file.
    foreach ($e in $entries) {
        if ($e.Path -notmatch '^[A-Za-z0-9._+/-]+$') { Fail "the entry name $($e.Path) holds a character that this script does not write" }
        if (Test-RuntimeName ([IO.Path]::GetFileName($e.Path))) { Fail "the zip would hold the HIP runtime file $($e.Path)" }
        $e | Add-Member -NotePropertyName Sha256 -NotePropertyValue $(if ($e.File) { (Get-FileHash -LiteralPath $e.File -Algorithm SHA256).Hash } else { Get-Sha256 $e.Bytes })
    }
    $paths = [string[]]@($entries | ForEach-Object Path)
    if (@($paths | Select-Object -Unique).Count -ne $paths.Count) { Fail 'two entries of the zip have the same name' }
    [Array]::Sort($paths, [StringComparer]::OrdinalIgnoreCase)
    $byPath = @{}
    foreach ($e in $entries) { $byPath[$e.Path] = $e }
    $m = New-Object 'System.Collections.Generic.List[string]'
    $m.Add('{')
    $m.Add("  `"name`": $(ConvertTo-JsonText $name),")
    $m.Add("  `"llama.cpp`": $(ConvertTo-JsonText $base),")
    $m.Add("  `"amdgpu-wddm`": $(ConvertTo-JsonText $commit),")
    $m.Add("  `"release`": $(ConvertTo-JsonText $Release),")
    $m.Add('  "runtime_not_included": [')
    for ($i = 0; $i -lt $runtimeUsed.Count; $i++) { $m.Add("    $(ConvertTo-JsonText $runtimeUsed[$i])$(if ($i + 1 -lt $runtimeUsed.Count) { ',' })") }
    $m.Add('  ],')
    $m.Add('  "files": {')
    for ($i = 0; $i -lt $paths.Count; $i++) {
        $m.Add("    $(ConvertTo-JsonText $paths[$i]): $(ConvertTo-JsonText $byPath[$paths[$i]].Sha256)$(if ($i + 1 -lt $paths.Count) { ',' })")
    }
    $m.Add('  }')
    $m.Add('}')
    Add-Text 'manifest.json' (($m -join "`r`n") + "`r`n")
    $last = $entries[$entries.Count - 1]
    $last | Add-Member -NotePropertyName Sha256 -NotePropertyValue (Get-Sha256 $last.Bytes)
    $byPath[$last.Path] = $last

    # ---- the zip ---------------------------------------------------------------------------------------------------
    # The entries are sorted by name, the patches in the order they apply, and every entry carries the time of the
    # commit, so the same inputs give the same entries. The bytes of the zip also depend on the compressor of the .NET
    # that runs this script.
    $order = @{}
    for ($i = 0; $i -lt $patchSet.Count; $i++) { $order["patches/$($patchSet[$i].Patch)"] = 'patches/{0:D3}' -f $i }
    $order['patches/llama-patches.json'] = 'patches/900'
    $order["patches/$diffName"] = 'patches/901'
    $all = [string[]]@($byPath.Keys)
    $sortKeys = [string[]]@($all | ForEach-Object { if ($order.ContainsKey($_)) { $order[$_] } else { $_ } })
    [Array]::Sort($sortKeys, $all, [StringComparer]::OrdinalIgnoreCase)
    $stamp = [DateTimeOffset]::FromUnixTimeSeconds($commitTime - ($commitTime % 2))
    [void][IO.Directory]::CreateDirectory($OutDir)
    $fs = [IO.File]::Open($partial, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $zip = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create, $false)
        try {
            foreach ($path in $all) {
                $e = $byPath[$path]
                $z = $zip.CreateEntry("$name/$path", [IO.Compression.CompressionLevel]::Optimal)
                $z.LastWriteTime = $stamp
                $s = $z.Open()
                try {
                    if ($e.File) { $in = [IO.File]::OpenRead($e.File); try { $in.CopyTo($s) } finally { $in.Dispose() } }
                    else { $s.Write($e.Bytes, 0, $e.Bytes.Length) }
                } finally { $s.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $fs.Dispose() }
    # The zip reads back with the same entries and the same SHA-256 of each.
    $zip = [IO.Compression.ZipFile]::OpenRead($partial)
    try {
        $names = @($zip.Entries | ForEach-Object FullName)
        if (($names -join '|') -cne (@($all | ForEach-Object { "$name/$_" }) -join '|')) { Fail 'the zip does not read back with the entries it was written with' }
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            foreach ($z in $zip.Entries) {
                $s = $z.Open()
                try { $h = (($sha.ComputeHash($s) | ForEach-Object { $_.ToString('X2') }) -join '') } finally { $s.Dispose() }
                if ($h -ne $byPath[$z.FullName.Substring($name.Length + 1)].Sha256) { Fail "$($z.FullName) reads back with another SHA-256" }
            }
        } finally { $sha.Dispose() }
    } finally { $zip.Dispose() }
    Move-Item -LiteralPath $partial -Destination $zipPath -Force
} finally {
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
$zipInfo = Get-Item -LiteralPath $zipPath
"asset: $zipPath"
"size: $($zipInfo.Length) bytes"
"sha256: $((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
"entries: $($all.Count) in the folder $name"
"ggml-hip.dll SHA-256 $hipHash"
"runtime not included: $($runtimeUsed -join ', ')"
"local changes: $($patchSet.Count) patches, $($changed.Count) files changed, $($added.Count) added and $($removed.Count) deleted against $short"
