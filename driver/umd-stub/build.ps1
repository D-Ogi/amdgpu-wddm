# Builds bc250umd.dll, the M7 stage A user-mode display driver stub, without a WDK or SDK installation: headers
# come from the NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\bc250kmd_cli\build.ps1, plus the WDK's um headers, which is where d3dumddi.h and d3d10umddi.h live.
#
#   pwsh driver\umd-stub\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250umd
#
# The DLL is not signed here and not packaged here. It is packaged by the driver build, which takes this -Out as
# its -UmdStub and writes a second package with the INF's UserModeDriverName block live and this DLL in the
# catalog; without that switch the block stays commented out (driver\kmd\README.md, "The two runs").

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/GS-', '/LD', '/DWIN32_LEAN_AND_MEAN',
    '/wd4201', '/wd4214',           # nameless unions and bit fields in the WDK's own headers, as in driver\kmd

    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$wdk\Include\$KitVersion\um", "/I$wdk\Include\$KitVersion\shared",
    "/Fo$Out\bc250umd.obj", "/Fe$Out\bc250umd.dll",
    # No C runtime at all: this DLL is loaded into every process that touches Direct3D, and it has nothing to
    # initialize. DllMain is the entry point directly, which is legal because its signature is the DLL entry
    # point's signature.
    (Join-Path $here 'bc250umd.c'), '/link', '/NODEFAULTLIB', '/ENTRY:DllMain',
    "/DEF:$(Join-Path $here 'bc250umd.def')", "/LIBPATH:$sdkLib\um\x64", 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|\.exp') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

Get-Item "$Out\bc250umd.dll" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
# The three names the runtime looks up, printed from the built DLL: the point of this stub is that they are there.
$exports = & (Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe') '/nologo' '/exports' "$Out\bc250umd.dll" |
    Where-Object { $_ -match '\bOpenAdapter' }
$exports | ForEach-Object { "  $($_.Trim())" }
foreach ($name in 'OpenAdapter', 'OpenAdapter10', 'OpenAdapter10_2') {
    if (-not ($exports | Where-Object { $_ -match "\b$name\s*$" })) { throw "$name is not exported" }
}
