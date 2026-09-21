# Builds and runs the M4 shim replay test, and compile-checks the same shim with the WDK kernel
# flags the miniport uses. Host-side only: nothing here touches the lab machine.
#
#   pwsh driver\shim\test\run.ps1
#   pwsh driver\shim\test\run.ps1 -Out P:\BC-250\scratch\m4shim -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\m4shim), never into the repository
# and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\m4shim',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250,
    [switch]$SkipTrace
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'
$amdhdr = Join-Path $repo 'third_party\linux-amdgpu'
$evid = Join-Path $repo 'evidence\linux\2026-09-21-E03-init-trace'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'    # the headers and the import libraries
                                                               # come from two different packages

$objUser = Join-Path $Out 'obj-user'
$objKern = Join-Path $Out 'obj-kernel'
New-Item -ItemType Directory -Force $Out, $objUser, $objKern | Out-Null
Remove-Item "$objUser\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# /W4 /WX for our own code. The imported AMD files are compiled unmodified, so the two warnings
# they raise are turned off from here and nowhere else:
#   C4244  u64 -> u32 in the SR-IOV arm of soc15_common.h's register macros (AGP_BOT/TOP and the
#          system aperture are programmed from 64-bit MC addresses; the arm is never taken).
#   C4701  mmhub_v2_0_update_medium_grain_clock_gating() reads `def`/`data` on the 2.1.x branch
#          where it never assigned them. Upstream defect, unreachable on GC 10.1.3, not our call.
$importWarn = @('/wd4244', '/wd4701')

$shimSources = @((Join-Path $shim 'shim.c'), (Join-Path $shim 'bc250_gmc.c'))
$importSources = (Get-ChildItem (Join-Path $imports '*.c')).FullName
$testSources = @((Join-Path $shim 'test\backend_trace.c'), (Join-Path $shim 'test\replay.c'))

$incUser = @("/I$shim\include", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, host replay test)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS') +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $shimSources + $testSources)
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi') + $importWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $importSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\replay.exe", "/PDB:$Out\replay.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

# The same sources with the flags of driver\kmd\build.ps1. Compile only: there is no kernel backend
# for bc250_shim_rreg/wreg yet, so nothing to link. BC250_SHIM_KERNEL picks the kernel variant of
# the fixed-width types (the WDK's km CRT has no <stdint.h>).
Write-Host 'compile (kernel flags, same sources, no link)'
$incKern = @("/I$shim\include", "/I$imports", "/I$amdhdr",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um")
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214', '/DBC250_SHIM_KERNEL',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $shimSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $importWarn + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $importSources)

# The reference trace, made with the recipe in the test's header comment.
$traceFile = Join-Path $Out 'trace-gart.txt'
if (-not $SkipTrace -or -not (Test-Path $traceFile)) {
    Write-Host 'extract the reference trace'
    & python (Join-Path $repo 'tools\trace\extract_phase.py') (Join-Path $evid 'amdgpu-events.txt') `
        --match 'GCVM|GCMC|MMVM|MMMC' --until 0.26 | Set-Content -Encoding ascii $traceFile
    if ($LASTEXITCODE -ne 0) { throw 'extract_phase.py failed' }
}

Write-Host 'run'
$argv = @((Join-Path $evid 'sweep-before-run1-GC-complete-then-hang.log'),
          (Join-Path $evid 'sweep-before-run2-nonGC.log'),
          $traceFile)
if ($Verbose250) { $argv += '-v' }
& "$Out\replay.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "replay.exe exit code $code"
exit $code
