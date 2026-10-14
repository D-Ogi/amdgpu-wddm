# Builds bc250kmd_cli.exe without a WDK or SDK installation: headers and import libraries come from the
# SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\bc250rd\build.ps1, minus the driver and the signing.
#
#   pwsh tools\win\bc250kmd_cli\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250kmd_cli
#
# -Lab builds the lab flavour (BD-167): AMDGPU_WDDM_LAB_FLAVOUR adds the raw diagnostic verbs (read, write, memory,
# gart, psp, gfx, ih, fence, ib, dcn, dcnflip, sdmacopy, sdmaib, fbdump and the VRAM verbs), whose escapes only a lab
# KMD has. Without it the CLI and the DLL are the release flavour. test-flavour.ps1 checks the built CLI either way.

param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('x64','x86')][string]$Arch = 'x64',
    [switch]$Lab
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\$Arch\cl.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
# /Brepro on cl and link: a content hash where they write a time. /FC with /d1trimfile: __FILE__ and the name that
# MSVC gives an anonymous namespace (a hash of the source path) see only the path below the repository. Two builds
# of one commit in two directories give the same bytes (docs/design/reproducible-builds.md).
$repro = @('/Brepro', '/FC', "/d1trimfile:$repo")
$flavour = @(if ($Lab) { '/DAMDGPU_WDDM_LAB_FLAVOUR=1' })

# bc250kmd_cli.c repeats the driver's BC250_STAGE numbers. Refuse to build a tool that names them wrongly.
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python -m unittest discover -s $here 2>&1 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { throw 'test_stages.py failed: the stage table no longer matches driver/kmd/bc250kmd.h' }
} else {
    Write-Warning 'python not found, skipping test_stages.py (stage table against driver/kmd/bc250kmd.h)'
}

# Host tests of the exports bc250control.dll gives the monitor, with the D3DKMT calls replaced: no adapter is
# opened and no escape is sent. Each throws on a failure, so no DLL is built past one.
foreach ($test in 'test-start-health.ps1', 'test-confirm.ps1', 'test-telemetry.ps1') {
    & (Join-Path $here $test) -Root (Split-Path (Split-Path ([IO.Path]::GetFullPath($Kits)))) `
        -Out (Join-Path $Out ('tests\' + [IO.Path]::GetFileNameWithoutExtension($test)))
}

$env:INCLUDE = ''; $env:LIB = ''
$stateFlags = $repro + @('/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared")
$stateObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cl -Out "$Out\state" -Flags $stateFlags)
# log_test.c: the tool's log paging against a fake driver, i.e. which escapes take the adapter lock (BD-054).
# Refuse to build a tool whose `log summary` would again idle the GPU once per page.
# Both tests replace hardware access. The wave test also decodes fixed values
# and checks damaged saved snapshots. Build no tool past a failing test.
$testOut = Join-Path $Out 'test'
New-Item -ItemType Directory -Force $testOut | Out-Null
foreach ($source in 'log_test', 'waves_test', 'state_command_test') {
$testObjects = @(); if ($source -eq 'log_test') { $testObjects = $stateObjects }
& $cl @repro @flavour @('/nologo', '/W4', '/WX', '/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$testOut\$source.obj", "/Fe$testOut\$source.exe",
    (Join-Path $here "$source.c")) @testObjects @('/link', '/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")",
    "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed on $source.c ($LASTEXITCODE)" }
& "$testOut\$source.exe" 2>&1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw "$source.c failed" }
}

& $cl @repro @flavour @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250kmd_cli.obj", "/Fe$Out\bc250kmd_cli.exe",
    (Join-Path $here 'bc250kmd_cli.c')) @stateObjects @('/link', '/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")",
    "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
& (Join-Path $here 'test-flavour.ps1') -Cli "$Out\bc250kmd_cli.exe" -Flavour $(if ($Lab) { 'lab' } else { 'release' }) |
    ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'bc250kmd_cli.exe is not the flavour this build asked for (test-flavour.ps1)' }

$stateExports = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cl -Out "$Out\state-exports" -Flags $stateFlags -Exports)

# The monitor uses the same adapter selection and typed request code in-process.
& $cl @repro @flavour @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250control.obj", "/Fe$Out\bc250control.dll",
    (Join-Path $here 'bc250kmd_cli.c')) @stateExports @('/link', '/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")",
    "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch", 'gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib')
if ($LASTEXITCODE -ne 0) { throw "control DLL build failed ($LASTEXITCODE)" }

# Both products must match the selected target; callers package the matching DLL.
foreach ($product in 'bc250kmd_cli.exe','bc250control.dll') {
    $image = [IO.File]::ReadAllBytes((Join-Path $Out $product))
    $pe = [BitConverter]::ToInt32($image, 0x3c)
    $machine = [BitConverter]::ToUInt16($image, $pe + 4)
    $expected = if ($Arch -eq 'x64') { 0x8664 } else { 0x14c }
    if ($machine -ne $expected) { throw "$product is not $Arch" }
}
Get-Item "$Out\bc250kmd_cli.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
