# Builds and runs the caps-blob parity test: driver/contract/bc250_umd_private.h fed with unit A's
# measured values, through Mesa's real src/amd/common/ac_gpu_info.c. Also compile-checks the
# contract header with the WDK kernel flags the miniport uses.
#
#   pwsh driver\contract\test\run.ps1
#   pwsh driver\contract\test\run.ps1 -Out $env:BC250_ROOT\scratch\contract -Mesa $env:BC250_ROOT\ref\mesa
#
# Host-side only: no lab machine, no hardware, no driver load. Everything is written under -Out
# (default <BC250_ROOT>\scratch\contract), never into the repository and never onto drive C:.
# BC250_ROOT is the workspace root: the environment variable, else the parent directory of this
# repository.
#
# Mesa is NOT forked. Two headers that meson would generate are generated here with Mesa's own
# scripts, into $Out\gen, and ac_gpu_info.c is compiled from the checkout unmodified. If this
# script ever needs to patch a Mesa source, that is the signal to stop and say so, not to fork.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\contract",
    [string]$Mesa = "$Root\ref\mesa",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Regenerate
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$contract = Join-Path $repo 'driver\contract'

if (-not (Test-Path $Mesa)) {
    throw "Mesa checkout not found at $Mesa. See driver\contract\README.md for the clone command."
}

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'    # headers and import libraries come
                                                               # from two different packages

$gen = Join-Path $Out 'gen'
$objUser = Join-Path $Out 'obj-user'
$objKern = Join-Path $Out 'obj-kernel'
New-Item -ItemType Directory -Force $Out, $gen, (Join-Path $gen 'util\format'), $objUser, $objKern | Out-Null
Remove-Item "$objUser\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# --- the two headers meson would generate ---------------------------------------------------
# amdgfxregs.h: src/amd/common/meson.build:76-82, the same argument list and the same file order.
# u_format_gen.h: src/util/format/meson.build:19-26.
$regsH = Join-Path $gen 'amdgfxregs.h'
$fmtH = Join-Path $gen 'util\format\u_format_gen.h'
if ($Regenerate -or -not (Test-Path $regsH)) {
    Write-Host 'generate amdgfxregs.h (Mesa makeregheader.py)'
    $jsons = @('gfx6', 'gfx7', 'gfx8', 'gfx81', 'gfx9', 'gfx940', 'gfx10', 'gfx103', 'gfx11',
        'gfx115', 'gfx12', 'pkt3', 'gfx10-rsrc', 'gfx11-rsrc', 'gfx12-rsrc',
        'registers-manually-defined') | ForEach-Object { Join-Path $Mesa "src\amd\registers\$_.json" }
    & python (Join-Path $Mesa 'src\amd\registers\makeregheader.py') @jsons --sort address --guard AMDGFXREGS_H |
        Set-Content -Encoding ascii $regsH
    if ($LASTEXITCODE -ne 0) { throw 'makeregheader.py failed' }
}
if ($Regenerate -or -not (Test-Path $fmtH)) {
    Write-Host 'generate u_format_gen.h (Mesa u_format_table.py)'
    & python (Join-Path $Mesa 'src\util\format\u_format_table.py') (Join-Path $Mesa 'src\util\format\u_format.yaml') --enums |
        Set-Content -Encoding ascii $fmtH
    if ($LASTEXITCODE -ne 0) { throw 'u_format_table.py failed' }
}

# --- compile ----------------------------------------------------------------------------------
# /std:c11 is needed for _Alignas in Mesa's src/util/u_atomic.h:374; MSVC's default C mode rejects
# it. Nothing else about Mesa needs a flag.
$incMesa = @("/I$gen", "/I$Mesa\src\amd\common", "/I$Mesa\src\amd", "/I$Mesa\src", "/I$Mesa\include")
$incSdk = @("/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")
$incOurs = @("/I$contract", "/I$contract\third_party", "/I$contract\uapi-shim", "/I$here")

# Mesa's own sources are compiled unmodified, so their warnings are turned off from here and
# nowhere else:
#   C4319  'ac_gpu_info.c:1296' zero-extending a ~32-bit value to 64 bits. Upstream, harmless.
#   C4101 / C4189  unused locals behind #ifdef arms that Windows does not take.
$mesaWarn = @('/wd4319', '/wd4101', '/wd4189', '/wd4244', '/wd4245', '/wd4267')

Write-Host 'compile (Mesa, unmodified)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/std:c11', '/W3', '/Od', '/Zi',
        '/D_CRT_SECURE_NO_WARNINGS') + $mesaWarn + $incMesa + $incSdk +
    @("/Fo$objUser\", "/Fd$objUser\cl.pdb",
      (Join-Path $Mesa 'src\amd\common\ac_gpu_info.c'),
      # amd_family.c defines ac_get_family_name (:11) and ac_get_ip_type_string (:207), which
      # ac_print_gpu_info() needs for the --radeon-info dump. Both used to be abort-stubs;
      # compiling Mesa's real file is cheaper than pretending, and keeps the dump's "name =" and
      # "IP ..." lines identical to what RADV prints.
      (Join-Path $Mesa 'src\amd\common\amd_family.c')))

Write-Host 'compile (ours)'
# /W4 /WX for our own code, the same bar as driver\shim.
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/std:c11', '/W4', '/WX', '/Od', '/Zi',
        '/D_CRT_SECURE_NO_WARNINGS') + $incOurs + $incSdk +
    @("/Fo$objUser\", "/Fd$objUser\cl.pdb",
      (Join-Path $here 'bc250_caps_unitA.c'), (Join-Path $here 'bc250_caps_test.c')))
# The Mesa-facing translation unit needs Mesa's headers as well as ours, and inherits Mesa's
# warning exemptions because struct radeon_info is Mesa's shape, not ours.
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/std:c11', '/W4', '/WX', '/Od', '/Zi',
        '/D_CRT_SECURE_NO_WARNINGS') + $mesaWarn + $incOurs + $incMesa + $incSdk +
    @("/Fo$objUser\", "/Fd$objUser\cl.pdb",
      (Join-Path $here 'bc250_caps_mesa.c'), (Join-Path $here 'bc250_caps_stubs.c')))

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\bc250_caps_test.exe", "/PDB:$Out\bc250_caps_test.pdb") +
    (Get-ChildItem "$objUser\*.obj").FullName)

# --- the contract header with the miniport's flags ---------------------------------------------
# bc250_umd_private.h has to compile inside bc250kmd too, which is a different compiler mode: no
# <stdint.h>, /kernel, /Zp8. The compile-time asserts in the header are the real payload here; if
# the packing differed in kernel mode they would fire.
Write-Host 'compile (kernel flags, the contract header only, no link)'
$kernProbe = Join-Path $Out 'kernel_probe.c'
@'
/* Generated by driver\contract\test\run.ps1. Proves the contract headers compile, and that their
   compile-time size and offset asserts hold, under the flags driver\kmd\build.ps1 uses.
   bc250_umd_submit.h is here for the same reason as bc250_umd_private.h: the miniport has to be
   able to parse these blobs, so they must compile in kernel mode with /Zp8. */
#include "bc250_umd_private.h"
#include "bc250_umd_submit.h"
static struct bc250_umd_private probe;
static struct bc250_umd_alloc_private   alloc_probe;
static struct bc250_umd_context_private ctx_probe;
static struct bc250_umd_submit_private  submit_probe;
unsigned long bc250_contract_kernel_probe(void);
unsigned long bc250_contract_kernel_probe(void)
{
    return probe.size + (unsigned long)sizeof(probe) +
           alloc_probe.size + (unsigned long)sizeof(alloc_probe) +
           ctx_probe.size + (unsigned long)sizeof(ctx_probe) +
           submit_probe.size + (unsigned long)sizeof(submit_probe);
}
'@ | Set-Content -Encoding ascii $kernProbe
$incKern = @("/I$contract", "/I$contract\third_party", "/I$contract\uapi-shim",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\um")
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214', '/DBC250_CONTRACT_KERNEL',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C',
    '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb", $kernProbe))

Write-Host 'run'
Write-Host ''
& "$Out\bc250_caps_test.exe"
$code = $LASTEXITCODE
Write-Host ''
Write-Host "bc250_caps_test.exe exit code $code"

# --- compare against the device ----------------------------------------------------------------
# Everything above checks the blob against itself and against Mesa. This checks it against unit A:
# compare_radv_info.py diffs the radeon_info this test derives against the one RADV derived on the
# real GPU (RADV_DEBUG=info). It is the only step here that can catch a value that is internally
# consistent and still wrong, which has happened.
#
# Its result is reported but does NOT change this script's exit code. It passes today; keeping it
# advisory means a Mesa bump in ref\mesa, which changes which fields exist, cannot break the build
# for everyone working on the driver. Read the summary - it is the only check here that compares
# against the device.
$radv = Join-Path $repo 'evidence\linux\2026-09-21-E14-vulkan-compute-reference\radv-info.txt'
$compare = Join-Path $here 'compare_radv_info.py'
if ((Test-Path $radv) -and (Test-Path $compare) -and (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Host ''
    Write-Host 'compare against unit A (RADV_DEBUG=info)'
    Write-Host ''
    & python $compare --reference $radv --exe "$Out\bc250_caps_test.exe" |
        Select-String -Pattern '^ \d+ matched|^OK:|^FAIL:|^OPEN:|^ MISMATCH|^ ONLY' |
        ForEach-Object { Write-Host "  $($_.Line)" }
    Write-Host ''
    Write-Host "  full report: python $compare"
} elseif (-not (Test-Path $radv)) {
    Write-Host ''
    Write-Host "  (no device comparison: $radv not found)"
}

exit $code
