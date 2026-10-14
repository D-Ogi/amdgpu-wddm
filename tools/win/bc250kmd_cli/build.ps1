# Builds bc250kmd_cli.exe without a WDK or SDK installation: headers and import libraries come from the
# SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same flow as
# tools\win\bc250rd\build.ps1, minus the driver and the signing.
#
#   pwsh tools\win\bc250kmd_cli\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250kmd_cli
#
# -Lab builds the lab flavour (BD-167): AMDGPU_WDDM_LAB_FLAVOUR adds the raw diagnostic verbs (read, write, memory,
# gart, psp, gfx, ih, fence, ib, dcn, dcnflip, sdmacopy, sdmaib, fbdump and the VRAM verbs), whose escapes only a lab
# KMD has. Without it the CLI and the DLL are the release flavour. test-verbs.ps1 runs each of those verbs against a
# fake adapter in the flavour of the build before the CLI is compiled, and test-flavour.ps1 checks the built CLI.

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
$labDefine = @(if ($Lab) { '/DAMDGPU_WDDM_LAB_FLAVOUR=1' })

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
# /Z7 here as well, so the state client's objects carry their debug information into the tool's PDB.
$stateFlags = $repro + @('/O2', '/MT', '/Z7', '/D_CRT_SECURE_NO_WARNINGS',
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
& $cl @repro @labDefine @('/nologo', '/W4', '/WX', '/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
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
# verbs_test.c: each raw diagnostic verb with a valid argument shape against a fake adapter (BD-167). A lab build
# must reach the handler's escape, and a release build must answer "unknown command" and open no adapter.
& (Join-Path $here 'test-verbs.ps1') -Kits $Kits -Out (Join-Path $testOut 'verbs') -KitVersion $KitVersion -Arch $Arch `
    -Lab:$Lab | ForEach-Object { Write-Host $_ }

# The tool ships, so it gets a PDB: /Z7 in the object and /DEBUG at the link (archived beside the candidate,
# named in the manifest by GUID and age).
& $cl @repro @labDefine @('/nologo', '/W4', '/WX', '/O2', '/MT', '/Z7', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250kmd_cli.obj", "/Fe$Out\bc250kmd_cli.exe",
    (Join-Path $here 'bc250kmd_cli.c')) @stateObjects @('/link', '/Brepro', '/DEBUG', '/OPT:REF', '/OPT:ICF',
    '/PDBALTPATH:%_PDB%', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")",
    "/LIBPATH:$sdkLib\ucrt\$Arch", "/LIBPATH:$sdkLib\um\$Arch",
    'gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib') |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
& (Join-Path $here 'test-flavour.ps1') -Cli "$Out\bc250kmd_cli.exe" -Flavour $(if ($Lab) { 'lab' } else { 'release' }) |
    ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'bc250kmd_cli.exe is not the flavour this build asked for (test-flavour.ps1)' }

$stateExports = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cl -Out "$Out\state-exports" -Flags $stateFlags -Exports)

# The monitor uses the same adapter selection and typed request code in-process.
& $cl @repro @labDefine @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/Z7', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/Fo$Out\bc250control.obj", "/Fe$Out\bc250control.dll",
    (Join-Path $here 'bc250kmd_cli.c')) @stateExports @('/link', '/Brepro', '/DEBUG', '/OPT:REF', '/OPT:ICF',
    '/PDBALTPATH:%_PDB%', "/LIBPATH:$(Join-Path $msvc.FullName "lib\$Arch")",
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
