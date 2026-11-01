# make-rocm-payload.ps1 - the directory that attach-rocm-venv.ps1 attaches from (blaslt\README.md, "The attach step").
#
# Inputs:
#   -Face         our amdhip64_7.dll, the AMD ABI face of the HIP runtime (build-amd-abi.ps1).
#   -AmdFace      AMD's amdhip64_7.dll of the TheRock build that the face replaces (rocm_sdk_core, _rocm_sdk_core\bin).
#   -FrontRecord  blaslt-front.json of build-blaslt-front.ps1: the front, AMD's libhipblaslt.dll it was built for, and
#                 the result of its host test. A front whose host test did not pass is refused.
#   -TheRock      the version of rocm_sdk_core and rocm_sdk_libraries, for example 10.3.0a20261030.
#   -Out          the payload directory. The release package carries it as payload\tools\hip\rocm, so the installer
#                 puts it in <install root>\tools\hip\rocm with the HIP runtime. That directory is not on PATH: no
#                 process outside an attached venv loads these two DLLs.
#   -Version      optional: the package version. The copies of our two DLLs in -Out then get the version resource of
#   -Flavour      the release (tools\build\version-stamp.ps1, BD-166) before payload.json is written. A release build
#                 (build-rocm-payload.ps1) gives both; a lab kit gives neither.
# Output: amdhip64_7.dll, libhipblaslt.dll, attach-rocm-venv.ps1 and payload.json under -Out.
# payload.json (schema 2) names the TheRock build and the SHA-256 of AMD's two DLLs that ours replace; those are fixed,
# because AMD's files are not in the payload. Of our two DLLs it gives build_sha256, the bytes in -Out after the stamp:
# the release signs every PE file of the package after this step, so the installed bytes differ, and
# attach-rocm-venv.ps1 takes the files of the payload directory as ours.
param(
    [Parameter(Mandatory = $true)][string]$Face,
    [Parameter(Mandatory = $true)][string]$AmdFace,
    [Parameter(Mandatory = $true)][string]$FrontRecord,
    [Parameter(Mandatory = $true)][string]$TheRock,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Version = '',
    [ValidateSet('release', 'lab')][string]$Flavour = 'release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
function Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Every input is read and checked before the first file is written: a refusal leaves -Out as it was.
$record = Get-Content -LiteralPath $FrontRecord -Raw | ConvertFrom-Json
if ($record.host_test.exit -ne 0) { throw "$FrontRecord says the host test of the front exited $($record.host_test.exit)" }
$front = $record.front.path
if ((Sha $front) -ne $record.front.sha256) { throw "$front is not the front that $FrontRecord names ($($record.front.sha256))" }
if ($TheRock -notmatch '^\d+\.\d+\.\d+') { throw "-TheRock '$TheRock' is not a version" }
$faceSha = Sha $Face
$amdFaceSha = Sha $AmdFace
if ($faceSha -eq $amdFaceSha) { throw "-Face $Face has the bytes of AMD's DLL ($amdFaceSha), so it is not our face" }
$attach = Join-Path $PSScriptRoot 'attach-rocm-venv.ps1'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
foreach ($src in @($Face, $front, $attach)) {
    if ((Split-Path -Parent (Resolve-Path -LiteralPath $src).Path) -ieq $Out) { throw "$src is inside -Out $Out; give another -Out" }
}

Copy-Item -LiteralPath $Face -Destination (Join-Path $Out 'amdhip64_7.dll') -Force
Copy-Item -LiteralPath $front -Destination (Join-Path $Out 'libhipblaslt.dll') -Force
Copy-Item -LiteralPath $attach -Destination $Out -Force
if ((Sha (Join-Path $Out 'amdhip64_7.dll')) -ne $faceSha -or (Sha (Join-Path $Out 'libhipblaslt.dll')) -ne $record.front.sha256) {
    throw "a copy into $Out does not have the bytes of its source"
}
if ($Version) {
    # version-stamp.ps1 needs PowerShell 7; this script also runs in Windows PowerShell 5.1 for the lab kits.
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if (-not $pwsh) { throw "-Version needs PowerShell 7 (pwsh) for tools\build\version-stamp.ps1" }
    $stamp = Join-Path $PSScriptRoot '..\..\tools\build\version-stamp.ps1'
    & $pwsh.Source -NoProfile -File $stamp -Version $Version -Flavour $Flavour `
        (Join-Path $Out 'amdhip64_7.dll') (Join-Path $Out 'libhipblaslt.dll')
    if ($LASTEXITCODE -ne 0) { throw "version-stamp.ps1 exited $LASTEXITCODE" }
}

$manifest = [ordered]@{
    schema       = 2
    therock      = $TheRock
    face         = [ordered]@{ file = 'amdhip64_7.dll'; amd_sha256 = $amdFaceSha; build_sha256 = (Sha (Join-Path $Out 'amdhip64_7.dll')) }
    blaslt_front = [ordered]@{
        file = 'libhipblaslt.dll'; amd_sha256 = $record.amd_blaslt.sha256; build_sha256 = (Sha (Join-Path $Out 'libhipblaslt.dll'))
        amd_exports = $record.amd_blaslt.exports; served_names = @($record.served_names)
    }
}
[IO.File]::WriteAllText((Join-Path $Out 'payload.json'), ($manifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
Get-ChildItem -LiteralPath $Out -File | ForEach-Object { Write-Host ('  {0}  {1}' -f (Sha $_.FullName), $_.Name) }
