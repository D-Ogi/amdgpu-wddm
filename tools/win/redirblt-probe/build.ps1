# Builds redirblt-probe.exe without a WDK or SDK installation: headers and import libraries come from the SDK
# NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\kmtprobe\build.ps1. The build ends with --help and a SHA-256 of the artifact, the receipt a lab
# runner pins against.
#
#   pwsh tools\win\redirblt-probe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\redirblt-probe

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\redirblt-probe",
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
$root = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# The LB7A magic is repeated from the driver; a copy that drifts is a build failure, not a lab puzzle.
$tool = Join-Path $here 'redirblt-probe.c'
$source = Join-Path $here '..\..\..\driver\kmd\gdi_private.h'
$pattern = '#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC\s+(0x[0-9A-Fa-f]+)'
$inSource = Select-String -Path $source -Pattern $pattern | Select-Object -First 1
$inTool = Select-String -Path $tool -Pattern $pattern | Select-Object -First 1
if ($null -eq $inSource -or $null -eq $inTool) { throw 'allocation private magic not found in both files' }
if ($inSource.Matches[0].Groups[1].Value -ne $inTool.Matches[0].Groups[1].Value) {
    throw "allocation private magic differs: gdi_private.h $($inSource.Matches[0].Groups[1].Value), redirblt-probe.c $($inTool.Matches[0].Groups[1].Value)"
}
Write-Host "  allocation private magic $($inTool.Matches[0].Groups[1].Value) matches gdi_private.h"

# A reviewed artifact is never lost to a rebuild: the existing probe is kept under retained\ by its full hash.
$previous = Join-Path $Out 'redirblt-probe.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\redirblt-probe-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\redirblt-probe-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
# The decisions first: redirblt_policy.h is pinned by a host-only test (no window, no DWM, no D3DKMT). A
# failing case fails the build.
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\redirblt-policy-test.obj", "/Fe$Out\redirblt-policy-test.exe",
    (Join-Path $here 'redirblt-policy-test.c'), '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64", 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed on the policy test ($LASTEXITCODE)" }
& "$Out\redirblt-policy-test.exe" | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'policy test failed' }

& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\redirblt-probe.obj", "/Fe$Out\redirblt-probe.exe",
    $tool, '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64",
    'gdi32.lib', 'user32.lib', 'dwmapi.lib', 'version.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\redirblt-probe.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
Get-Item "$Out\redirblt-probe.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
