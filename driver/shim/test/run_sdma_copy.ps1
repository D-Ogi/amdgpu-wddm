# Builds and runs the SDMA copy/fill packet check: does bc250_sdma_copy.c write exactly what
# amdgpu's sdma_v5_0_emit_copy_buffer()/emit_fill_buffer() would (ADR 0013), dword for dword,
# splitting where amdgpu_copy_buffer()/amdgpu_ttm_fill_mem() would split. Host-side only: nothing
# here touches the lab machine, and nothing here is a positive control against real hardware - that
# is BC250_ESCAPE_RUN_SDMACOPY, run through bc250kmd_cli on unit A.
#
#   pwsh driver\shim\test\run_sdma_copy.ps1
#   pwsh driver\shim\test\run_sdma_copy.ps1 -Out P:\BC-250\scratch\build\sdma-copy -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\build\sdma-copy), never into the
# repository and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\sdma-copy',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250
)

$ErrorActionPreference = 'Stop'
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

# The code under test and the shim files it needs to link, the same four driver\kmd\build.ps1
# compiles for the miniport (bc250_sdma_copy.c, bc250_sdma.c, bc250_ring.c, bc250_nbio.c). No
# backend_mem.c and no backend_trace.c: this file supplies its own plain shim backend (a working
# allocator, a no-op register file) so it links against driver/shim alone, exactly as
# sdma_faults.c does for the same reason.
#   C4245  as in run_gfx.ps1/run_sdma_faults.ps1: AMD's SDMA packet macros yield a signed int with
#          bit 31 set.
$packetWarn = @('/wd4245')
$packetSources = @('bc250_sdma_copy.c', 'bc250_sdma.c', 'bc250_ring.c') | ForEach-Object { Join-Path $shim $_ }
# shim.c: the amdgpu_sriov_* stubs soc15_common.h's register macros name in the branch this part
# never takes.
$plainSources = @('bc250_nbio.c', 'shim.c') | ForEach-Object { Join-Path $shim $_ }
$testSources = @((Join-Path $shim 'test\sdma_copy_packets.c'))

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, SDMA copy/fill packets)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $packetWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $plainSources + $testSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\sdma_copy_packets.exe", "/PDB:$Out\sdma_copy_packets.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
$argv = @()
if ($Verbose250) { $argv += '-v' }
& "$Out\sdma_copy_packets.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "sdma_copy_packets.exe exit code $code"
exit $code
