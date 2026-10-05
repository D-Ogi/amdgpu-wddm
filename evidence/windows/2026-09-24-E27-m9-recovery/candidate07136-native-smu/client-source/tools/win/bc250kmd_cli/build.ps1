# Builds bc250kmd_cli.exe without a WDK or SDK installation: headers and import libraries come from the
# SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\bc250rd\build.ps1, minus the driver and the signing.
#
#   pwsh tools\win\bc250kmd_cli\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd_cli

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null

# bc250kmd_cli.c repeats the driver's BC250_STAGE numbers. Refuse to build a tool that names them wrongly.
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python -m unittest discover -s $here 2>&1 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { throw 'test_stages.py failed: the stage table no longer matches driver/kmd/bc250kmd.h' }
} else {
    Write-Warning 'python not found, skipping test_stages.py (stage table against driver/kmd/bc250kmd.h)'
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250kmd_cli.obj", "/Fe$Out\bc250kmd_cli.exe",
    (Join-Path $here 'bc250kmd_cli.c'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

# The monitor uses the same adapter selection and typed request code in-process.
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250control.obj", "/Fe$Out\bc250control.dll",
    (Join-Path $here 'bc250kmd_cli.c'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64", 'gdi32.lib', 'setupapi.lib', 'advapi32.lib')
if ($LASTEXITCODE -ne 0) { throw "control DLL build failed ($LASTEXITCODE)" }

Get-Item "$Out\bc250kmd_cli.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
