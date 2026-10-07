# Builds d3d9probe.exe (the Direct3D 9 path probe) without a WDK or SDK installation: headers and import libraries come from the SDK NuGet packages
# unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as tools\win\d3d11bench\build.ps1; the
# build ends with --help and a SHA-256 of the artifact, the receipt a lab runner pins against.
#
#   pwsh tools\win\d3d9probe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\d3d9probe

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\d3d9probe",
    [string]$KitVersion = '10.0.26100.0',
    # x86: a 32-bit build, the D3D9 path of a WoW64 process (D3D9On12 on the UserModeDriverNameWow D3D12 entry).
    # Pass another -Out: the file name does not change.
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
$previous = Join-Path $Out 'd3d9probe.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\d3d9probe-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\d3d9probe-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt", "/Fo$Out\d3d9probe.obj",
    "/Fe$Out\d3d9probe.exe", (Join-Path $here 'd3d9probe.cpp'), '/link',
    "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")", "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'psapi.lib', 'user32.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.cpp$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\d3d9probe.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
Get-Item "$Out\d3d9probe.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
