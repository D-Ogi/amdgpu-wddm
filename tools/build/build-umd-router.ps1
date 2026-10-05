# Builds bc250d3d_router.dll (driver/umd/router: the desktop route and the AppRouter application policy) and its
# host-test binaries: five UMD doubles (fake-*.dll) and test-router.exe. test-umd-router.ps1 runs the host gate on
# the output.
#
#   pwsh tools\build\build-umd-router.ps1 [-OutputDir <dir>] [-VsInstall <dir>]
#
# The recipe is the one that built the registered router 674AD261 (2026-10-01, driver/umd/router/README.md):
# VS 2022 vcvars64 (MSVC 14.44.35207, cl 19.44.35221) with the WDK 10.0.26100.0 um and shared headers in front of
# INCLUDE; /O2 /MD /W4 /WX /Zi, linked /DEBUG /OPT:REF /OPT:ICF with /PDBALTPATH:%_PDB%, so the DLL names its PDB
# by file name only and carries no build path. No /Brepro: the PE and debug-directory timestamps and the PDB
# signature differ on every build (see the README for what a rebuild compares).
param([string]$OutputDir, [string]$VsInstall)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir = Join-Path $root 'scratch\build\umd-router' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir.Contains(' ')) { throw "OutputDir must not contain spaces (cl /Fo takes it with a trailing backslash): $OutputDir" }
$src = Join-Path $repo 'driver\umd\router'
$contract = Join-Path $repo 'driver\contract'
$wdk = Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0'
if (-not (Test-Path -LiteralPath "$wdk\um\d3d10umddi.h")) { throw "WDK headers not found under $wdk" }
$obj = Join-Path $OutputDir 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $OutputDir; $env:TMP = $OutputDir
    $install = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
    $env:INCLUDE = "$wdk\um;$wdk\shared;$env:INCLUDE"
    Write-Host "Visual Studio: $install, cl $(Get-ClVersion)"
    Push-Location $OutputDir
    try {
        & cl.exe /nologo /O2 /MD /W4 /WX /Zi /LD "/Fo$obj\" "/Fd$obj\router-vc.pdb" "$src\router.cpp" "/Fe:$OutputDir\bc250d3d_router.dll" /link /DEBUG /OPT:REF /OPT:ICF "/PDB:$OutputDir\bc250d3d_router.pdb" '/PDBALTPATH:%_PDB%'
        if ($LASTEXITCODE -ne 0) { throw 'router build failed' }
        # The doubles: one source, a tag per role (cpu, hosted, app), one without OpenAdapter10_2, one that fails.
        $doubles = @(
            @('fake-cpu', @('/DFAKE_TAG=0xC0')),
            @('fake-hosted', @('/DFAKE_TAG=0x60')),
            @('fake-app', @('/DFAKE_TAG=0xA0')),
            @('fake-nooa102', @('/DFAKE_TAG=0xB0', '/DFAKE_NO_OA102')),
            @('fake-fail', @('/DFAKE_TAG=0xFA', '/DFAKE_FAIL')))
        foreach ($d in $doubles) {
            $name = $d[0]; $defines = $d[1]
            & cl.exe /nologo /O2 /MD /W4 /WX /LD @defines "/Fo$obj\$name.obj" "$src\tests\fake-umd.cpp" "/Fe:$OutputDir\$name.dll"
            if ($LASTEXITCODE -ne 0) { throw "$name build failed" }
        }
        & cl.exe /nologo /O2 /MD /W4 /WX /EHsc /std:c++17 "/I$contract" "/Fo$obj\test-router.obj" "$src\tests\test-router.cpp" "/Fe:$OutputDir\test-router.exe"
        if ($LASTEXITCODE -ne 0) { throw 'test-router build failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
'{0}  bc250d3d_router.dll' -f (Get-FileHash -LiteralPath (Join-Path $OutputDir 'bc250d3d_router.dll')).Hash
