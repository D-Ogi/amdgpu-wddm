# Builds and runs the M6 shim replay test - the interrupt ring, its decode, and a fence that raises
# one end-of-pipe interrupt - and compile-checks the same shim sources with the WDK kernel flags the
# miniport uses. Host-side only: nothing here touches the lab machine.
#
#   pwsh driver\shim\test\run_ih.ps1
#   pwsh driver\shim\test\run_ih.ps1 -Out P:\BC-250\scratch\m5-gfx -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\m5-gfx), never into the repository and
# never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\m5-gfx',
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
$libdrm = Join-Path $repo 'third_party\libdrm'      # the gfx10 dispatch shader, imported verbatim
$evid = Join-Path $repo 'evidence\linux\2026-09-21-E03-init-trace'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objUser = Join-Path $Out 'obj-ih-user'
$objKern = Join-Path $Out 'obj-ih-kernel'
New-Item -ItemType Directory -Force $Out, $objUser, $objKern | Out-Null
Remove-Item "$objUser\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# Same flags and the same one exception as run_gfx.ps1; see that script for why C4245 is off for the
# files that build PM4 packets.
$packetWarn = @('/wd4245')
$importWarn = @('/wd4244', '/wd4701')

$packetSources = @('bc250_ring.c', 'bc250_gfx.c', 'bc250_sdma.c',
                   'bc250_dispatch.c') | ForEach-Object { Join-Path $shim $_ }
$plainSources = @('shim.c', 'bc250_gmc.c', 'bc250_gart.c', 'bc250_nbio.c',
                  'bc250_irq.c', 'bc250_ih.c') | ForEach-Object { Join-Path $shim $_ }
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c') | ForEach-Object { Join-Path $imports $_ }
$testSources = @('test\backend_trace.c', 'test\backend_mem.c', 'test\replay_ih.c') | ForEach-Object { Join-Path $shim $_ }

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr", "/I$libdrm",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, host replay test)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $packetWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $plainSources + $testSources)
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi') + $importWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $importSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\replay_ih.exe", "/PDB:$Out\replay_ih.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

# The same shim sources with the flags of driver\kmd\build.ps1. Compile only. bc250_ih.c has three
# functions the miniport calls at DISPATCH_LEVEL, so it compiling under /kernel is not a formality.
Write-Host 'compile (kernel flags, same shim sources, no link)'
$incKern = @("/I$shim\include", "/I$imports", "/I$amdhdr", "/I$libdrm",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um")
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214', '/DBC250_SHIM_KERNEL',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $packetWarn + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $plainSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $importWarn + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $importSources)

# The reference window: the whole of navi10_ih_irq_init() on unit A, twenty accesses at 0.2528 s.
# OSSSYS and NBIO only - the IH block comes up with the common IP, long before the CP, so no GC
# register is touched between these timestamps.
$traceFile = Join-Path $Out 'trace-ih.txt'
if (-not $SkipTrace -or -not (Test-Path $traceFile)) {
    Write-Host 'extract the reference trace 0.2520 to 0.2540'
    & python (Join-Path $repo 'tools\trace\extract_phase.py') (Join-Path $evid 'amdgpu-events.txt') `
        --match '^(OSSSYS|NBIO)\.' --reads --no-fold --precision 6 --since 0.2520 --until 0.2540 |
        Set-Content -Encoding ascii $traceFile
    if ($LASTEXITCODE -ne 0) { throw 'extract_phase.py failed' }
}

Write-Host 'run'
$argv = @((Join-Path $evid 'sweep-before-run1-GC-complete-then-hang.log'),
          (Join-Path $evid 'sweep-before-run2-nonGC.log'),
          $traceFile)
if ($Verbose250) { $argv += '-v' }
& "$Out\replay_ih.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "replay_ih.exe exit code $code"
exit $code
