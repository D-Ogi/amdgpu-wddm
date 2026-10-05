# Builds and runs the paging-node packet check (ADR 0008 stage D, docs/design/paging-node.md): does
# bc250_sdma_paging.c write exactly what bc250_sdma_emit_copy_linear()/emit_fill() would for a
# TRANSFER_VIRTUAL/FILL_VIRTUAL operation, and does the room check answer
# BC250_SDMA_PAGING_INSUFFICIENT - writing nothing - when the buffer is too small. Host-side only:
# nothing here touches the lab. -KmdRouting extracts actual KMD builders, page
# walkers and publication with field-level WDK models. -OmitLogicalCommit is a
# deliberately failing control, never a candidate driver modification.
#
#   pwsh driver\shim\test\run_paging.ps1
#   pwsh driver\shim\test\run_paging.ps1 -Out P:\BC-250\scratch\build\paging -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\build\paging), never into the
# repository and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\paging',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250,
    [switch]$KmdRouting,
    [switch]$OmitApertureCommit,
    [switch]$OmitLogicalCommit
)

$ErrorActionPreference = 'Stop'
if ($OmitLogicalCommit -and -not $KmdRouting) { throw 'Mutation requires -KmdRouting' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'
$amdhdr = Join-Path $repo 'third_party\linux-amdgpu'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objUser = Join-Path $Out 'obj-user'
New-Item -ItemType Directory -Force $Out, $objUser | Out-Null
Remove-Item "$objUser\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# The code under test and the shim files it needs to link, the same set driver\kmd\build.ps1
# compiles for the miniport (bc250_sdma_paging.c, bc250_sdma_copy.c, bc250_sdma.c, bc250_ring.c). No
# backend_mem.c and no backend_trace.c: this file supplies its own plain shim backend, exactly as
# run_sdma_copy.ps1 does for the same reason.
#   C4245  as in run_gfx.ps1/run_sdma_copy.ps1: AMD's SDMA packet macros yield a signed int with
#          bit 31 set.
$packetWarn = @('/wd4245')
$packetSources = @('bc250_sdma_paging.c', 'bc250_sdma_copy.c', 'bc250_sdma.c', 'bc250_ring.c') | ForEach-Object { Join-Path $shim $_ }
# shim.c: the amdgpu_sriov_* stubs soc15_common.h's register macros name in the branch this part
# never takes.
$plainSources = @('bc250_nbio.c', 'shim.c') | ForEach-Object { Join-Path $shim $_ }
$testSources = @((Join-Path $shim 'test\paging_packets.c'))

if ($KmdRouting) {
    $plainSources += Join-Path $repo 'driver\kmd\paging_pt_shadow.c'
    $plainSources += Join-Path $repo 'driver\kmd\paging_private.c'
    $plainSources += Join-Path $repo 'driver\kmd\paging_aperture_state.c'
    $generated = Join-Path $Out 'paging-route.c'
    $generatorArgs=@($repo,$generated)
    if ($OmitLogicalCommit) { $generatorArgs+='--omit-logical-commit' }
    if ($OmitApertureCommit) { $generatorArgs+='--omit-aperture-commit' }
    & python (Join-Path $repo 'experiments\E27-m9-inference\generate-paging-route-test.py') @generatorArgs
    if ($LASTEXITCODE -ne 0) { throw 'route extraction failed' }
    $testSources = @($generated, (Join-Path $repo 'driver\kmd\paging_mc.c'), (Join-Path $repo 'driver\kmd\paging_window.c'), (Join-Path $repo 'driver\kmd\paging_stream.c'))
    $packetSources += @((Join-Path $shim 'bc250_gart.c'), (Join-Path $shim 'bc250_pte.c'))
}
$incUser = @("/I$repo\driver\kmd","/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, paging-node packets)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $packetWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $plainSources + $testSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\paging_packets.exe", "/PDB:$Out\paging_packets.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
$argv = @()
if ($Verbose250) { $argv += '-v' }
& "$Out\paging_packets.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "paging_packets.exe exit code $code"
exit $code
