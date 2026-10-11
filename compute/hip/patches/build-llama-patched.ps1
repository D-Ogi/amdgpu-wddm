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
    [switch]$ValidationTools
)
$ErrorActionPreference = 'Stop'
$sourceText = Get-Content (Join-Path $Source 'ggml/src/ggml-cuda/common.cuh') -Raw
if ($sourceText -notmatch '#ifndef BC250_HIP_NO_DEVICE_PRINTF') {
    throw 'Apply llama-no-device-printf.patch to the reviewed source copy first.'
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
$targets = @('ggml-hip')
if ($ValidationTools) {
    $options += '-DLLAMA_BUILD_COMMON:BOOL=ON', '-DLLAMA_BUILD_TESTS:BOOL=ON', '-DLLAMA_BUILD_TOOLS:BOOL=ON'
    $targets += 'test-backend-ops', 'llama-completion', 'llama-bench', 'llama-perplexity'
}
$options += "-DCMAKE_CXX_FLAGS=$originalCxxFlags -DBC250_HIP_NO_DEVICE_PRINTF=1"
if ($Bc250AllocationCap) {
    $allocationSource = Get-Content (Join-Path $Source 'ggml/src/ggml-cuda/ggml-cuda.cu') -Raw
    if ($allocationSource -notmatch 'BC250_HIP_MAX_ALLOC_BYTES') {
        throw 'Apply llama-bc250-allocation-cap.patch before enabling the measured allocation cap.'
    }
    $options += "-DCMAKE_CXX_FLAGS=$originalCxxFlags -DBC250_HIP_NO_DEVICE_PRINTF=1 -DBC250_HIP_MAX_ALLOC_BYTES=0xfffc0000ULL"
}
$previousRocm = $env:ROCM_PATH
try {
    $env:ROCM_PATH = $RocmRoot
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
