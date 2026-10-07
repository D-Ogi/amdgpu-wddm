# Builds d3d10probe.exe (the Direct3D 10.0 path probe, BD-081) without a WDK or SDK installation: headers and import
# libraries come from the SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same
# flow as tools\win\d3d9probe\build.ps1; the build ends with --help and a SHA-256 of the artifact, the receipt a lab
# runner pins against. The shader compiler is not linked: the probe loads the system's d3dcompiler_47.dll at run time.
#
#   pwsh tools\win\d3d10probe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\d3d10probe
#   pwsh tools\win\d3d10probe\build.ps1 -Kits ... -Arch x86 -Out $env:BC250_ROOT\scratch\build\d3d10probe-x86

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\d3d10probe",
    [string]$KitVersion = '10.0.26100.0',
    # x86: a 32-bit build, the D3D10 path of a WoW64 process (the x86 router, UserModeDriverNameWow). Pass another
    # -Out: the file name does not change.
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
# d3d10umddi.h (the DDI arguments that --trace-entry logs) is a WDK header. The headers do not depend on the target
# architecture, so the x64 package serves the x86 build too, as for the x86 router.
$wdk = Join-Path $Kits "microsoft.windows.wdk.x64\c\Include\$KitVersion"
if (-not (Test-Path -LiteralPath "$wdk\um\d3d10umddi.h")) { throw "WDK headers not found under $wdk" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$previous = Join-Path $Out 'd3d10probe.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\d3d10probe-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\d3d10probe-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE', '/Brepro',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/I$wdk\um", "/I$wdk\shared",
    "/Fo$Out\d3d10probe.obj",
    "/Fe$Out\d3d10probe.exe", (Join-Path $here 'd3d10probe.cpp'), '/link', '/Brepro',
    "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")", "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'd3d10.lib', 'dxgi.lib', 'psapi.lib', 'user32.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\d3d10probe.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
$machine = [BitConverter]::ToUInt16([IO.File]::ReadAllBytes("$Out\d3d10probe.exe"), [BitConverter]::ToInt32([IO.File]::ReadAllBytes("$Out\d3d10probe.exe"), 0x3C) + 4)
$want = if ($Arch -eq 'x86') { 0x14C } else { 0x8664 }
if ($machine -ne $want) { throw ('d3d10probe.exe machine 0x{0:X}, want 0x{1:X}' -f $machine, $want) }
Get-Item "$Out\d3d10probe.exe" | ForEach-Object { '{0,9}  {1}  {2}  sha256 {3}' -f $_.Length, $_.Name, $Arch, (Get-FileHash -LiteralPath $_.FullName).Hash }
