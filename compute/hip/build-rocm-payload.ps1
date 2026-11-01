#Requires -Version 7.0
# build-rocm-payload.ps1 - the release build of payload\tools\hip\rocm: our HIP runtime and hipBLASLt front for the
# TheRock ROCm Python packages, with the step that attaches them to a venv (blaslt\README.md, "The attach step").
#
# The recipe of the rows extra-rocm-release and extra-rocm-lab of release-sources.json. Steps, in order:
#   1. -TheRockInput holds what AMD's TheRock build gives (therock.json names it): include\ of _rocm_sdk_core, AMD's
#      amdhip64_7.dll and libhipblaslt.dll as pip installed them, and imp-hip.tsv, the HIP import census. The two
#      AMD DLLs must have the SHA-256 that therock.json records. Nothing of that directory ships.
#   2. build-amd-abi.ps1 -Out <Out>\face: the face amdhip64_7.dll over layer 1 (-Bc250hsaLib), with its gates.
#   3. build-blaslt-front.ps1 -Out <Out>\front: the front libhipblaslt.dll for AMD's libhipblaslt.dll, with its host
#      test.
#   4. make-rocm-payload.ps1 -Out <Out>\rocm -Version -Flavour: the payload directory. Our two DLLs get the version
#      resource of the release there, and payload.json (schema 2) is written after it.
#   5. test_blaslt_front of step 3 again, on the stamped front of the payload.
# Outputs under -Out: rocm\amdhip64_7.dll, rocm\libhipblaslt.dll, rocm\attach-rocm-venv.ps1, rocm\payload.json, and
# the PDBs face\amdhip64_7.pdb and front\libhipblaslt.pdb. The host test of the attach step
# (tests\host\test-attach-rocm-venv.ps1) is a test of the release row, not a step of this recipe.
param(
    [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl',
    [string]$MsvcReason = '',
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$Kits = '',
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Version,
    [ValidateSet('release', 'lab')][string]$Flavour = 'release',
    [Parameter(Mandatory = $true)][string]$Bc250hsaLib,
    [Parameter(Mandatory = $true)][string]$TheRockInput,
    [string]$PreviousPayloadManifest = '',
    [string]$PreviousPayloadSha256 = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
function Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$shell = (Get-Process -Id $PID).Path

$in = (Resolve-Path -LiteralPath $TheRockInput).Path
$rock = Get-Content -LiteralPath (Join-Path $in 'therock.json') -Raw | ConvertFrom-Json
$amdFace = Join-Path $in 'amdhip64_7.dll'
$amdLt = Join-Path $in 'libhipblaslt.dll'
foreach ($pair in @(@($amdFace, $rock.amdhip64_7_sha256), @($amdLt, $rock.libhipblaslt_sha256))) {
    $have = Sha $pair[0]
    if ($have -ne $pair[1]) { throw "$($pair[0]) is $have, and therock.json of -TheRockInput names $($pair[1])" }
}
foreach ($need in @('include\hip\hip_runtime_api.h', 'imp-hip.tsv')) {
    if (-not (Test-Path -LiteralPath (Join-Path $in $need))) { throw "-TheRockInput $in has no $need" }
}
if (-not (Test-Path -LiteralPath $Bc250hsaLib)) { throw "-Bc250hsaLib $Bc250hsaLib does not exist" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
& $shell -NoProfile -File (Join-Path $PSScriptRoot '..\..\tools\release\test-rocm-files.ps1') -Out $Out
if ($LASTEXITCODE -ne 0) { throw "test-rocm-files.ps1 exited $LASTEXITCODE" }
Write-Host "TheRock $($rock.therock), $Flavour flavour, version $Version"

$common = @('-Compiler', $Compiler, '-Root', $Root)
if ($MsvcReason) { $common += @('-MsvcReason', $MsvcReason) }
if ($Kits) { $common += @('-Kits', $Kits) }
$lab = @(if ($Flavour -eq 'lab') { '-Lab' })

$face = Join-Path $Out 'face'
& $shell -NoProfile -File (Join-Path $PSScriptRoot 'build-amd-abi.ps1') @common -Out $face `
    -AmdInclude (Join-Path $in 'include') -Census (Join-Path $in 'imp-hip.tsv') -Bc250hsaLib $Bc250hsaLib @lab
if ($LASTEXITCODE -ne 0) { throw "build-amd-abi.ps1 exited $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath (Join-Path $face 'amdhip64_7.dll'))) { throw "build-amd-abi.ps1 left no $face\amdhip64_7.dll" }

$front = Join-Path $Out 'front'
& $shell -NoProfile -File (Join-Path $PSScriptRoot 'build-blaslt-front.ps1') @common -Out $front -AmdBlasLt $amdLt @lab
if ($LASTEXITCODE -ne 0) { throw "build-blaslt-front.ps1 exited $LASTEXITCODE" }

$payload = Join-Path $Out 'rocm'
& $shell -NoProfile -File (Join-Path $PSScriptRoot 'make-rocm-payload.ps1') -Face (Join-Path $face 'amdhip64_7.dll') `
    -AmdFace $amdFace -FrontRecord (Join-Path $front 'blaslt-front.json') -TheRock $rock.therock -Out $payload `
    -Version $Version -Flavour $Flavour -PreviousPayloadManifest $PreviousPayloadManifest -PreviousPayloadSha256 $PreviousPayloadSha256
if ($LASTEXITCODE -ne 0) { throw "make-rocm-payload.ps1 exited $LASTEXITCODE" }
foreach ($f in @('amdhip64_7.dll', 'libhipblaslt.dll', 'attach-rocm-venv.ps1', 'rocm-files.ps1', 'payload.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $f))) { throw "make-rocm-payload.ps1 left no $payload\$f" }
}

# 5. The host test of the front again, on the stamped copy that ships: the stamp rewrites the resource section.
$stamped = Join-Path $Out 'stamped-test'
if (Test-Path -LiteralPath $stamped) { Remove-Item -LiteralPath $stamped -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $front 'host-test') -Destination $stamped -Recurse
Copy-Item -LiteralPath (Join-Path $payload 'libhipblaslt.dll') -Destination (Join-Path $stamped 'libhipblaslt.dll') -Force
$text = & (Join-Path $front 'test_blaslt_front.exe') $stamped 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$text | Set-Content (Join-Path $Out 'test_blaslt_front_stamped.txt') -Encoding utf8
$passes = @($text | Where-Object { $_ -match '^PASS ' }).Count
$fails = @($text | Where-Object { $_ -match '^FAIL ' }).Count
if ($code -ne 0 -or $fails -or -not $passes) { throw "test_blaslt_front on the stamped front: exit $code, $passes PASS, $fails FAIL" }
Write-Host "  test_blaslt_front on the stamped front: $passes PASS lines (test_blaslt_front_stamped.txt)"
Write-Host "build-rocm-payload: $payload"
exit 0
