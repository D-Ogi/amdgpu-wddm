# Builds d3d12ace.exe (x64) with clang-cl, the project's user-mode compiler, without a WDK or SDK installation:
# headers, import libraries and dxc come from the SDK NuGet packages under -Kits, the C++ runtime from the installed
# Visual Studio. dxc compiles stage.hlsl to signed DXIL (cs_6_0, with its root signature), embedded as a C array.
#
# The build ends with --help, two usage checks, a self-test on the first hardware adapter of this PC (skipped when
# there is none) and the SHA-256 of the program, which a lab kit pins.
#
#   pwsh tools\win\d3d12ace\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
#   pwsh tools\win\d3d12ace\build.ps1 -Kits ... -Out $env:BC250_ROOT\scratch\build\d3d12ace -SkipSelfTest

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\d3d12ace",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Clang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe",
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$dxc = Join-Path $sdk "bin\$KitVersion\x64\dxc.exe"
foreach ($tool in @($Clang, $dxc, (Join-Path (Split-Path -Parent $dxc) 'dxil.dll'))) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool not found: $tool" }
}
Write-Host "  $((& $Clang --version | Select-Object -First 1))"
if ($LASTEXITCODE) { throw 'clang-cl version query failed' }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $here '..\..\..\..')).Path }
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$exe = Join-Path $Out 'd3d12ace.exe'
if (Test-Path -LiteralPath $exe) {
    $hash = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\d3d12ace-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\d3d12ace-$hash.exe"
}

# DXIL signed by the dxil.dll next to dxc, which the D3D12 runtime requires; the root signature is in the blob.
& $dxc -T cs_6_0 -E main -Vn g_stage -Fh (Join-Path $Out 'stage_dxil.h') (Join-Path $here 'stage.hlsl') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "dxc failed on stage.hlsl ($LASTEXITCODE)" }

$env:INCLUDE = ''; $env:LIB = ''
& $Clang @('-fuse-ld=lld', '-march=x86-64-v3', '-mtune=znver2', '/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17',
    '/D_CRT_SECURE_NO_WARNINGS', '/Brepro', "/I$Out",
    "/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt", "/imsvc$sdk\Include\$KitVersion\um",
    "/imsvc$sdk\Include\$KitVersion\shared", "/imsvc$sdk\Include\$KitVersion\winrt", "/Fo$Out\d3d12ace.obj", "/Fe$exe",
    (Join-Path $here 'd3d12ace.cpp'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64",
    "/LIBPATH:$sdkLib\um\x64", 'kernel32.lib', 'dxgi.lib', 'gdi32.lib', 'psapi.lib', 'bcrypt.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "clang-cl failed ($LASTEXITCODE)" }

& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "help check failed ($LASTEXITCODE)" }
& $exe --bad-option 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "usage check failed ($LASTEXITCODE)" }
& $exe --expect-node 1 2>$null | Out-Null
if ($LASTEXITCODE -ne 2) { throw "node value check failed ($LASTEXITCODE)" }

# The self-test runs steps 1 to 6 on the first hardware adapter of this PC with --expect-node any: the node packets
# are reported, not checked. Exit 5 (no hardware adapter) skips it. Any other failure stops the build: another
# vendor's driver passing every check is the positive control of the client itself.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    $log = Join-Path $Out 'selftest.log'
    & $exe --any-device --expect-node any --deadline 120 > $log 2>&1
    $code = $LASTEXITCODE
    if ($code -eq 5) {
        Write-Host "  self-test skipped: exit $code ($((Get-Content $log | Select-String 'FAIL' | Select-Object -First 1)))"
    } else {
        Get-Content $log | Where-Object { $_ -match '^(FAIL|PASS)|^d3d12ace |^adapter' } | ForEach-Object { Write-Host "    $_" }
        if ($code -ne 0) { throw "self-test failed ($code)" }
    }
}

Get-Item (Join-Path $Out 'stage_dxil.h'), $exe | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
