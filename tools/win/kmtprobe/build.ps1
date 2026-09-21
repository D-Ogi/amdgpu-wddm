# Builds kmtprobe.exe without a WDK or SDK installation: headers and import libraries come from the SDK
# NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\bc250kmd_cli\build.ps1.
#
#   pwsh tools\win\kmtprobe\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\kmtprobe

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = 'P:\BC-250\scratch\build\kmtprobe',
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

# kmtprobe.c repeats driver/kmd/wddm.c's allocation private blob. A tool that sends a blob the KMD refuses
# would look like a VidMm problem, so the magic is checked against the driver here, at build time.
$wddm = Join-Path $here '..\..\..\driver\kmd\wddm.c'
if (Test-Path $wddm) {
    $pattern = '#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC\s+(0x[0-9A-Fa-f]+)'
    $inDriver = (Select-String -Path $wddm -Pattern $pattern).Matches[0].Groups[1].Value
    $inTool = (Select-String -Path (Join-Path $here 'kmtprobe.c') -Pattern $pattern).Matches[0].Groups[1].Value
    if ($inDriver -ne $inTool) {
        throw "BC250_WDDM_ALLOCATION_PRIVATE_MAGIC differs: driver $inDriver, kmtprobe $inTool"
    }
    Write-Host "  allocation private magic $inTool matches driver\kmd\wddm.c"
} else {
    Write-Warning "driver\kmd\wddm.c not found next to the tool, skipping the private-blob magic check"
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\kmtprobe.obj", "/Fe$Out\kmtprobe.exe",
    (Join-Path $here 'kmtprobe.c'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

Get-Item "$Out\kmtprobe.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
