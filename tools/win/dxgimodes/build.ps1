# Builds dxgimodes.exe without a WDK or SDK installation: headers and import libraries come from the SDK
# NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\kmtprobe\build.ps1.
#
#   pwsh tools\win\dxgimodes\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\dxgi-modes\build

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = 'P:\BC-250\scratch\dxgi-modes\build',
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

$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP   # cl's temporary files stay off drive C:
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\dxgimodes.obj", "/Fe$Out\dxgimodes.exe",
    (Join-Path $here 'dxgimodes.cpp'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'dxgi.lib', 'gdi32.lib', 'user32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

Get-Item "$Out\dxgimodes.exe" | ForEach-Object { '{0,9}  {1}  {2}' -f $_.Length, $_.Name, (Get-FileHash $_.FullName).Hash }
