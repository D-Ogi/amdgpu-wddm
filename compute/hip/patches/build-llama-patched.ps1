# Rebuild a reviewed llama source snapshot with the diagnostic-only consumer patch.
# Requires the MSVC/Windows SDK build environment and a ROCm-shaped root already assembled
# with make-rocm-root.py. This script never runs a produced binary.
param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$RocmRoot,
    [Parameter(Mandatory=$true)][string]$OriginalCache,
    [Parameter(Mandatory=$true)][string]$CMake,
    [ValidateRange(1,4)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$sourceText = Get-Content (Join-Path $Source 'ggml/src/ggml-cuda/common.cuh') -Raw
if ($sourceText -notmatch '#ifndef BC250_HIP_NO_DEVICE_PRINTF') {
    throw 'Apply llama-no-device-printf.patch to the reviewed source copy first.'
}
$options = @()
$originalCxxFlags = ''
foreach ($line in Get-Content $OriginalCache) {
    if ($line -match '^((?:GGML_|LLAMA_|BC250_|BUILD_SHARED_LIBS|CMAKE_(?:C_COMPILER|CXX_COMPILER|RC_COMPILER|C_COMPILER_TARGET|CXX_COMPILER_TARGET|BUILD_TYPE|MAKE_PROGRAM|C_FLAGS.*|CXX_FLAGS.*))[^:]*):(BOOL|STRING|UNINITIALIZED|FILEPATH)=(.*)$') {
        if ($Matches[3] -match 'NOTFOUND$') { continue }
        if ($Matches[1] -eq 'CMAKE_CXX_FLAGS') { $originalCxxFlags = $Matches[3]; continue }
        $options += "-D$($Matches[1]):$($Matches[2])=$($Matches[3])"
    }
}
$options += "-DCMAKE_PREFIX_PATH=$RocmRoot"
$options += "-DCMAKE_CXX_FLAGS=$originalCxxFlags -DBC250_HIP_NO_DEVICE_PRINTF=1"
$previousRocm = $env:ROCM_PATH
try {
    $env:ROCM_PATH = $RocmRoot
    & $CMake -S $Source -B $Build -G Ninja @options
    if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
    & $CMake --build $Build --target ggml-hip -j $Jobs
    if ($LASTEXITCODE) { throw "ggml-hip build failed: $LASTEXITCODE" }
    Get-FileHash (Join-Path $Build 'bin/ggml-hip.dll') -Algorithm SHA256
} finally {
    $env:ROCM_PATH = $previousRocm
}
