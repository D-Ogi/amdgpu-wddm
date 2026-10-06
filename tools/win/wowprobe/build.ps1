# Builds wowprobe.exe (x86 for the WoW64 witness, x64 for its control) without a WDK or SDK installation, like
# tools\win\d3d11fl12\build.ps1: headers and import libraries from the SDK NuGet packages under -Kits
# (microsoft.windows.sdk.cpp.x86 for -Arch x86), the compiler from the installed Visual Studio. Ends with --help and
# the artifact's SHA-256, the receipt a lab runner pins against.
#
#   pwsh tools\win\wowprobe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Arch x86 -Out $env:BC250_ROOT\scratch\build\wowprobe-x86

param(
    [Parameter(Mandatory)][string]$Kits,
    [ValidateSet('x64', 'x86')][string]$Arch = 'x86',
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\wowprobe-$Arch",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
if (-not (Test-Path -LiteralPath "$sdkLib\um\$Arch\d3d11.lib")) { throw "$sdkLib\um\$Arch\d3d11.lib missing: unpack microsoft.windows.sdk.cpp.$Arch under -Kits" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\wowprobe.obj",
    "/Fe$Out\wowprobe.exe", (Join-Path $here 'wowprobe.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")", "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'd3d11.lib', 'dxgi.lib', 'gdi32.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\wowprobe.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
Get-Item "$Out\wowprobe.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
