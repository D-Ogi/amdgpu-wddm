# Rebuild a reviewed llama source snapshot with the HIP consumer patches and Flash Attention.
# Requires the MSVC/Windows SDK build environment and a ROCm-shaped root already assembled
# with make-rocm-root.py. This script never runs a produced binary.
param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$RocmRoot,
    [Parameter(Mandatory=$true)][string]$OriginalCache,
    [Parameter(Mandatory=$true)][string]$CMake,
    [ValidateRange(1,4)][int]$Jobs = 4,
    [switch]$Bc250AllocationCap,
    # The cap in bytes. The default is the measured per-allocation limit of the runtime. A smaller cap makes llama.cpp
    # split the model into more buffers, and each buffer then fits whichever segment group still has room: the
    # runtime places device memory in the carve-out while the local budget holds it and in system memory after that.
    # Decimal on purpose: PowerShell reads the hex literal 0xfffc0000 as the Int32 -262144, which no UInt64 takes.
    [UInt64]$Bc250AllocationCapBytes = 4294705152,
    [switch]$ValidationTools,
    [switch]$AssetTools,
    [switch]$NoLto,
    # Apply any patch of the complete set that the source copy does not carry yet, with git apply, and
    # then check the markers as usual. Without it the script changes no file and only checks, which is
    # how a frozen reviewed snapshot is built.
    [switch]$ApplyPatches,
    # Where the patch files are. Empty means this script's own directory, resolved below rather than in
    # the default, because $PSScriptRoot is not yet set in a param default under powershell.exe -File.
    [string]$PatchDir = '',
    [string]$Git = 'git'
)
$ErrorActionPreference = 'Stop'
if (-not $PatchDir) {
    $PatchDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
}

# The first lines of the output name this script, its SHA-256 and its arguments, and the configure line
# below lists every CMake option, so that the log of a build tells how its binaries were made.
# pack-llama-asset.ps1 copies the hash and the options line into PROVENANCE.txt of the release asset.
# The options line holds no path: the paths are on the inputs line.
$self = if ($PSCommandPath) { $PSCommandPath } else { $MyInvocation.MyCommand.Path }
"builder $((Get-FileHash -LiteralPath $self -Algorithm SHA256).Hash) $self"
$builderOptions = @("-Jobs $Jobs")
if ($Bc250AllocationCap) { $builderOptions += '-Bc250AllocationCap', "-Bc250AllocationCapBytes $Bc250AllocationCapBytes" }
foreach ($name in 'ValidationTools', 'AssetTools', 'NoLto', 'ApplyPatches') {
    if ((Get-Variable -Name $name -ValueOnly).IsPresent) { $builderOptions += "-$name" }
}
"builder options: $($builderOptions -join ' ')"
"builder inputs: -Source $Source -Build $Build -RocmRoot $RocmRoot -OriginalCache $OriginalCache -CMake $CMake -PatchDir $PatchDir -Git $Git"

# The complete set of source changes this HIP route needs, each with the marker that says it is in,
# in the order they apply: llama-patches.json next to this script. A source copy that is missing one
# of them builds and loads, and is wrong or slow: without the RDNA1 entry the device compiles
# mul_mat_vec_q_moe with a smaller __launch_bounds__ than the block the host launches (256 threads
# into a kernel built for 128), which is the expert path of every MoE model. This set once held two
# of the four changes of its first day, and the other two lived only in a reviewed source snapshot,
# so a rebuild driven from this directory produced that wrong binary and nothing said so. The marker,
# not the patch file, is the evidence that a change is in. The allocation cap is the one opt-in
# change: -Bc250AllocationCap turns it on in the compiler flags, so the source carries it in every
# build and the define decides. Every other change, the RDNA1 kernel ports included, is on.
$patchList = Join-Path $PatchDir 'llama-patches.json'
if (-not (Test-Path -LiteralPath $patchList)) { throw "missing patch list $patchList" }
$patchSet = @((Get-Content -LiteralPath $patchList -Raw | ConvertFrom-Json).patches | ForEach-Object {
    @{ Patch = [string]$_.patch; File = [string]$_.file; Marker = [string]$_.marker; What = [string]$_.what }
})
if (-not $patchSet.Count) { throw "the patch list $patchList names no patch" }
function Test-PatchMarker($item) {
    $path = Join-Path $Source $item.File
    if (-not (Test-Path -LiteralPath $path)) { throw "the source copy has no $($item.File)" }
    # -Raw gives nothing at all for an empty file, and a [string] cast of that is still not a string,
    # so the null is handled before any method call. An empty file carries no marker.
    $text = Get-Content -LiteralPath $path -Raw
    if ($null -eq $text) { return $false }
    return ([string]$text).Contains($item.Marker)
}
if ($ApplyPatches) {
    foreach ($item in $patchSet) {
        if (Test-PatchMarker $item) { "already in: $($item.Patch)"; continue }
        $patch = Join-Path $PatchDir $item.Patch
        if (-not (Test-Path -LiteralPath $patch)) { throw "missing patch file $patch" }
        & $Git -C $Source apply -p1 --verbose -- $patch
        if ($LASTEXITCODE) { throw "git apply failed for $($item.Patch): $LASTEXITCODE" }
        if (-not (Test-PatchMarker $item)) {
            throw "$($item.Patch) applied but its marker $($item.Marker) is not in $($item.File)"
        }
        "applied: $($item.Patch)"
    }
}
$missing = @($patchSet | Where-Object { -not (Test-PatchMarker $_) })
if ($missing.Count) {
    throw ("the source copy is missing " + $missing.Count + " of the " + $patchSet.Count +
        " HIP source changes: " + (($missing | ForEach-Object { "$($_.Patch) ($($_.What))" }) -join '; ') +
        ". Apply them to the reviewed source copy first, or pass -ApplyPatches.")
}
$options = @()
$originalCxxFlags = ''
foreach ($line in Get-Content $OriginalCache) {
    if ($line -match '^((?:GGML_|LLAMA_|BC250_|BUILD_SHARED_LIBS|CMAKE_(?:C_COMPILER|CXX_COMPILER|RC_COMPILER|C_COMPILER_TARGET|CXX_COMPILER_TARGET|BUILD_TYPE|MAKE_PROGRAM|C_FLAGS.*|CXX_FLAGS.*|HIP_FLAGS.*))[^:]*):(BOOL|STRING|UNINITIALIZED|FILEPATH)=(.*)$') {
        $optionName, $optionType, $optionValue = $Matches[1], $Matches[2], $Matches[3]
        if ($optionValue -match 'NOTFOUND$') { continue }
        if ($optionName -match '^CMAKE_(?:C|CXX|HIP)_FLAGS' -and $optionValue -match 'GGML_CUDA_NO_FA') {
            throw 'The input cache disables Flash Attention in compiler flags. Remove GGML_CUDA_NO_FA from the build configuration.'
        }
        # The bring-up cache disabled FA before the occupancy API was implemented.
        # Use the reviewed source defaults for quant types, also in an existing output cache.
        if ($optionName -match '^GGML_CUDA_FA(?:_ALL_QUANTS|_QUANTS)?$') { continue }
        if ($optionName -eq 'CMAKE_CXX_FLAGS') { $originalCxxFlags = $optionValue; continue }
        $options += "-D${optionName}:${optionType}=$optionValue"
    }
}
if ($originalCxxFlags -match 'BC250_HIP_MAX_ALLOC_BYTES') { throw 'Use an original cache without BC250_HIP_MAX_ALLOC_BYTES. Enable the cap only with -Bc250AllocationCap.' }
$options += "-DCMAKE_PREFIX_PATH=$RocmRoot"
$options += '-DGGML_CUDA_FA:BOOL=ON', '-UGGML_CUDA_FA_ALL_QUANTS', '-UGGML_CUDA_FA_QUANTS'
# Maximum optimisation by default: link-time optimisation of the ggml libraries, the HIP backend included.
# -NoLto keeps the input cache's setting; the lab-tested FA candidate FC5B2D65 was built that way.
if (-not $NoLto) { $options += '-DGGML_LTO:BOOL=ON' }
$targets = @('ggml-hip')
if ($ValidationTools) {
    $options += '-DLLAMA_BUILD_COMMON:BOOL=ON', '-DLLAMA_BUILD_TESTS:BOOL=ON', '-DLLAMA_BUILD_TOOLS:BOOL=ON'
    $targets += 'test-backend-ops', 'llama-completion', 'llama-bench', 'llama-perplexity'
}
if ($AssetTools) {
    # The programs of the release asset. llama-server and llama-cli need LLAMA_BUILD_SERVER. The web UI is neither
    # built (npm) nor downloaded (LLAMA_USE_PREBUILT_UI fetches it from Hugging Face during the build), so the
    # server offers its HTTP API only and the build reads no network.
    $options += '-DLLAMA_BUILD_COMMON:BOOL=ON', '-DLLAMA_BUILD_TOOLS:BOOL=ON', '-DLLAMA_BUILD_SERVER:BOOL=ON',
        '-DLLAMA_BUILD_UI:BOOL=OFF', '-DLLAMA_USE_PREBUILT_UI:BOOL=OFF'
    $targets += 'llama-server', 'llama-cli', 'llama-completion', 'llama-bench'
}
$targets = @($targets | Select-Object -Unique)
$options += "-DCMAKE_CXX_FLAGS=$originalCxxFlags -DBC250_HIP_NO_DEVICE_PRINTF=1"
if ($Bc250AllocationCap) {
    $allocationSource = Get-Content (Join-Path $Source 'ggml/src/ggml-cuda/ggml-cuda.cu') -Raw
    if ($allocationSource -notmatch 'BC250_HIP_MAX_ALLOC_BYTES') {
        throw 'Apply llama-bc250-allocation-cap.patch before enabling the measured allocation cap.'
    }
    $options += "-DCMAKE_CXX_FLAGS=$originalCxxFlags -DBC250_HIP_NO_DEVICE_PRINTF=1 -DBC250_HIP_MAX_ALLOC_BYTES=0x$($Bc250AllocationCapBytes.ToString('x'))ULL"
}
$previousRocm = $env:ROCM_PATH
try {
    $env:ROCM_PATH = $RocmRoot
    "cmake configure: -S $Source -B $Build -G Ninja " + (($options | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }) -join ' ')
    "cmake build targets: $($targets -join ' ')"
    & $CMake -S $Source -B $Build -G Ninja @options
    if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
    $configuredCache = Get-Content (Join-Path $Build 'CMakeCache.txt')
    if ($configuredCache -notcontains 'GGML_CUDA_FA:BOOL=ON' -or
        ($configuredCache | Where-Object { $_ -match '^CMAKE_(?:C|CXX|HIP)_FLAGS[^:]*:[^=]+=.*GGML_CUDA_NO_FA' })) {
        throw 'Configured cache does not enable Flash Attention. No build was started.'
    }
    & $CMake --build $Build --target @targets -j $Jobs
    if ($LASTEXITCODE) { throw "HIP consumer build failed: $LASTEXITCODE" }
    Get-FileHash (Join-Path $Build 'bin/ggml-hip.dll') -Algorithm SHA256
} finally {
    $env:ROCM_PATH = $previousRocm
}
