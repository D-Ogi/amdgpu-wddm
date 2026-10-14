# Builds gdi-cpuwrite.exe without a WDK or SDK installation: headers and import libraries come from the SDK
# NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\redirblt-probe\build.ps1. The build ends with --help and a SHA-256 of the artifact, the receipt a
# lab runner pins against.
#
#   pwsh tools\win\gdi-cpuwrite\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\gdi-cpuwrite

param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path })\scratch\build\gdi-cpuwrite",
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

# The tool repeats three things from the driver: the two private-data magic numbers and the placement rule it
# prints as POLICY. A copy that drifts is a build failure, not a lab puzzle.
$tool = Join-Path $here 'gdi-cpuwrite.c'
$source = Join-Path $here '..\..\..\driver\kmd\gdi_private.h'
foreach ($name in 'BC250_WDDM_ALLOCATION_PRIVATE_MAGIC', 'BC250_GDI_PRIVATE_MAGIC') {
    $pattern = "#define $name\s+(0x[0-9A-Fa-f]+)"
    $inSource = Select-String -Path $source -Pattern $pattern | Select-Object -First 1
    $inTool = Select-String -Path $tool -Pattern $pattern | Select-Object -First 1
    if ($null -eq $inSource -or $null -eq $inTool) { throw "$name not found in both files" }
    if ($inSource.Matches[0].Groups[1].Value -ne $inTool.Matches[0].Groups[1].Value) {
        throw "$name differs: gdi_private.h $($inSource.Matches[0].Groups[1].Value), gdi-cpuwrite.c $($inTool.Matches[0].Groups[1].Value)"
    }
    Write-Host "  $name $($inTool.Matches[0].Groups[1].Value) matches gdi_private.h"
}
# WddmGdiAllocationPolicy, line for line. The POLICY lines of the tool state what these four give for type 0
# and type 2; a change here must change those lines too.
$policy = @(
    'result\.CpuVisible=!Type \|\| Type==2;',
    'result\.Aperture=Type \? Type==2 : SharedCpu!=0;',
    'result\.Cached=Type \? Type==2 : \(CachedCpu && result\.Aperture\);',
    'result\.AccessedPhysically=!Type && !result\.Aperture;'
)
foreach ($line in $policy) {
    if (-not (Select-String -Path $source -Pattern $line -Quiet)) {
        throw "WddmGdiAllocationPolicy no longer has the line this tool reports: $line"
    }
}
Write-Host '  WddmGdiAllocationPolicy unchanged: the POLICY lines still hold'

# A reviewed artifact is never lost to a rebuild: the existing tool is kept under retained\ by its full hash.
$previous = Join-Path $Out 'gdi-cpuwrite.exe'
if (Test-Path -LiteralPath $previous) {
    $hash = (Get-FileHash -LiteralPath $previous).Hash
    $keep = Join-Path $Out "retained\gdi-cpuwrite-$hash.exe"
    if (-not (Test-Path -LiteralPath $keep)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
        Copy-Item -LiteralPath $previous -Destination $keep
    }
    Write-Host "  previous artifact retained as retained\gdi-cpuwrite-$hash.exe"
}

$env:INCLUDE = ''; $env:LIB = ''
& $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\gdi-cpuwrite.obj", "/Fe$Out\gdi-cpuwrite.exe",
    $tool, '/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64", 'gdi32.lib', 'kernel32.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

& "$Out\gdi-cpuwrite.exe" --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'help check failed' }
# The baseline class needs no adapter, so one real measurement runs here, on the development PC: a build that
# cannot measure ordinary memory is broken before it ever reaches the lab.
& "$Out\gdi-cpuwrite.exe" --classes sys --mib 4 --passes 1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'the baseline measurement failed' }
Get-Item "$Out\gdi-cpuwrite.exe" | ForEach-Object { '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
