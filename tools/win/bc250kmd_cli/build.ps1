# Builds bc250kmd_cli.exe without a WDK or SDK installation: headers and import libraries come from the
# SDK NuGet packages unpacked under -Kits, the C runtime from the installed Visual Studio. Same flow as
# tools\win\bc250rd\build.ps1, minus the driver and the signing.
#
#   pwsh tools\win\bc250kmd_cli\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out $env:BC250_ROOT\scratch\build\bc250kmd_cli
#
# The compiler is clang-cl with lld-link of LLVM 23.1.4 (-Compiler clang-cl, the default, through
# tools\build\umd-compiler.ps1: x86-64-v3, the CRT and SDK as system headers, the version on the console and in
# compiler.json), the compiler of the release copies that tools\win\amdgpu_wddm_control\build.ps1 builds from the same
# source. -Compiler msvc builds with cl and link of that Visual Studio and needs -MsvcReason. The host tests that this
# script runs compile with the same compiler.
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
    [switch]$Lab,
    [ValidateSet('clang-cl','msvc')][string]$Compiler = 'clang-cl',
    [string]$MsvcReason = '',
    [string]$VsInstall = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Kits = [IO.Path]::GetFullPath($Kits)
$workspace = Split-Path (Split-Path $Kits -Parent) -Parent
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
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
    & (Join-Path $here $test) -Root $workspace -Compiler $Compiler -MsvcReason $MsvcReason -VsInstall $VsInstall `
        -Out (Join-Path $Out ('tests\' + [IO.Path]::GetFileNameWithoutExtension($test)))
}

$tool = Set-Bc250ToolEnvironment $Kits $Arch $KitVersion $VsInstall
$cc = Resolve-Bc250UmdCompiler $workspace $Compiler $MsvcReason $Out $Arch $tool.Cl
# clang-cl appends _CL_ after /link and reads CL (Invoke-Bc250UmdCompiler); build-objects.ps1 calls the compiler itself.
if ($Compiler -eq 'clang-cl') { $env:CL = $null; $env:_CL_ = $null }
# /Brepro on the compiler and the linker: a content hash where they write a time. cl: /FC with /d1trimfile, so that
# __FILE__ and the name that MSVC gives an anonymous namespace (a hash of the source path) see only the path below the
# repository. clang-cl: -ffile-prefix-map over the repository in place of those two, the BD-268 compile switches, and
# lld-link with /pdbsourcepath (Get-Bc250ToolFlags). The products are compiled and linked in -Out with paths relative
# to it. Two builds of one commit in two directories give the same bytes (docs/design/reproducible-builds.md).
$repro = @('/Brepro', '/FC', "/d1trimfile:$repo")
$link = @('/Brepro') + @((Get-Bc250ReproducibleFlags $Compiler).Link)
function Get-Flags([string[]]$Own) { return @(Get-Bc250ToolFlags $Compiler ($repro + $Own) $Arch @($repo)) }
function Get-ObjectDir([string]$Name) { New-Item -ItemType Directory -Force (Join-Path $Out "obj\$Name") | Out-Null; return "/Foobj\$Name\" }
$libs = @('gdi32.lib', 'setupapi.lib', 'advapi32.lib', 'onecore.lib', 'uuid.lib')

# /Z7 here as well, so the state client's objects carry their debug information into the tool's PDB.
$stateFlags = Get-Flags @('/O2', '/MT', '/Z7', '/D_CRT_SECURE_NO_WARNINGS')
$stateObjects = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cc -Out "$Out\state" -Flags $stateFlags -RelativeObjects)
$stateRelative = @($stateObjects | ForEach-Object { [IO.Path]::GetRelativePath($Out, $_) })
# log_test.c: the tool's log paging against a fake driver, i.e. which escapes take the adapter lock (BD-054).
# Refuse to build a tool whose `log summary` would again idle the GPU once per page.
# Both tests replace hardware access. The wave test also decodes fixed values
# and checks damaged saved snapshots. Build no tool past a failing test.
$testOut = Join-Path $Out 'test'
New-Item -ItemType Directory -Force $testOut | Out-Null
$testFlags = Get-Flags ($labDefine + @('/nologo', '/W4', '/WX', '/Od', '/MT', '/D_CRT_SECURE_NO_WARNINGS'))
foreach ($source in 'log_test', 'waves_test', 'state_command_test') {
$testObjects = @(); if ($source -eq 'log_test') { $testObjects = $stateObjects }
Invoke-Bc250UmdCompiler $cc @testFlags "/Fo$testOut\" "/Fe$testOut\$source.exe" (Join-Path $here "$source.c") @testObjects `
    @('/link') @link @libs |
    ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
if ($LASTEXITCODE -ne 0) { throw "$Compiler failed on $source.c ($LASTEXITCODE)" }
& "$testOut\$source.exe" 2>&1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw "$source.c failed" }
}
# verbs_test.c: each raw diagnostic verb with a valid argument shape against a fake adapter (BD-167). A lab build
# must reach the handler's escape, and a release build must answer "unknown command" and open no adapter.
& (Join-Path $here 'test-verbs.ps1') -Kits $Kits -Out (Join-Path $testOut 'verbs') -KitVersion $KitVersion -Arch $Arch `
    -Lab:$Lab -Compiler $Compiler -MsvcReason $MsvcReason -VsInstall $VsInstall | ForEach-Object { Write-Host $_ }

# The tool ships, so it gets a PDB: /Z7 in the object and /DEBUG at the link (archived beside the candidate,
# named in the manifest by GUID and age).
$cliFlags = Get-Flags ($labDefine + @('/nologo', '/W4', '/WX', '/O2', '/MT', '/Z7', '/D_CRT_SECURE_NO_WARNINGS'))
Push-Location -LiteralPath $Out
try {
    Invoke-Bc250UmdCompiler $cc @cliFlags (Get-ObjectDir 'cli') '/Febc250kmd_cli.exe' (Join-Path $here 'bc250kmd_cli.c') @stateRelative `
        @('/link') @link @('/DEBUG', '/OPT:REF', '/OPT:ICF', '/PDBALTPATH:%_PDB%') @libs |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$Compiler failed ($LASTEXITCODE)" }
} finally { Pop-Location }
& (Join-Path $here 'test-flavour.ps1') -Cli "$Out\bc250kmd_cli.exe" -Flavour $(if ($Lab) { 'lab' } else { 'release' }) |
    ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw 'bc250kmd_cli.exe is not the flavour this build asked for (test-flavour.ps1)' }

$stateExports = @(& "$repo\tools\state\build-objects.ps1" -Compiler $cc -Out "$Out\state-exports" -Flags $stateFlags -Exports -RelativeObjects)
$exportsRelative = @($stateExports | ForEach-Object { [IO.Path]::GetRelativePath($Out, $_) })

# The monitor uses the same adapter selection and typed request code in-process. The import library stays beside the
# DLL: tools\win\bc250rd\build.ps1 links against it.
$dllFlags = Get-Flags ($labDefine + @('/nologo', '/W4', '/WX', '/O2', '/MT', '/LD', '/Z7', '/DBC250_CONTROL_DLL', '/D_CRT_SECURE_NO_WARNINGS'))
Push-Location -LiteralPath $Out
try {
    Invoke-Bc250UmdCompiler $cc @dllFlags (Get-ObjectDir 'dll') '/Febc250control.dll' (Join-Path $here 'bc250kmd_cli.c') @exportsRelative `
        @('/link') @link @('/DEBUG', '/OPT:REF', '/OPT:ICF', '/PDBALTPATH:%_PDB%', '/IMPLIB:bc250control.lib') @libs
    if ($LASTEXITCODE -ne 0) { throw "control DLL build failed ($LASTEXITCODE)" }
} finally { Pop-Location }

# Both products must match the selected target; callers package the matching DLL.
foreach ($product in 'bc250kmd_cli.exe','bc250control.dll') {
    $image = [IO.File]::ReadAllBytes((Join-Path $Out $product))
    $pe = [BitConverter]::ToInt32($image, 0x3c)
    $machine = [BitConverter]::ToUInt16($image, $pe + 4)
    $expected = if ($Arch -eq 'x64') { 0x8664 } else { 0x14c }
    if ($machine -ne $expected) { throw "$product is not $Arch" }
}
Get-Item "$Out\bc250kmd_cli.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
