# SPDX-License-Identifier: MIT
# Build the M15.12 deliberate-hang client (hang.cpp). Mirrors tools/win/d3d12queue/build.ps1:
# system cl from vswhere, the WDK/SDK NuGet headers under -Kits, no application-local runtime.
# The compute shader is compiled at runtime (D3DCompile cs_5_0), so there is no offline codegen.
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { 'P:\bc-250' })\scratch\build\hangclient",
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { 'P:\bc-250' }
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = Join-Path $root 'scratch\tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: keep the existing binary under retained\ by its hash.
$exe = Join-Path $Out 'bc250hang.exe'
if (Test-Path -LiteralPath $exe) {
    $h = (Get-FileHash -LiteralPath $exe).Hash
    $keep = Join-Path $Out "retained\bc250hang-$h.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $exe -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\bc250hang-$h.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\bc250hang.obj",
    "/Fe$exe", (Join-Path $here 'hang.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'dxguid.lib', 'user32.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

# --help touches no D3D: a safe post-build smoke check on any machine, including this dev PC.
& $exe --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
& $exe --bogus | Out-Null
if ($LASTEXITCODE -ne 2) { throw "invalid CLI accepted (expected exit 2)" }

Get-Item $exe | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
