# Builds and runs the host test of the PSP path (replay_psp.c), and compile-checks the same sources
# with the WDK kernel flags the miniport uses. Host-side only: nothing here touches the lab machine.
#
#   pwsh driver\shim\test\run_psp.ps1
#   pwsh driver\shim\test\run_psp.ps1 -Firmware P:\BC-250\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\m5psp), never into the repository and
# never onto drive C:. The firmware files are linux-firmware's amdgpu/cyan_skillfish2_*.bin; they
# are not part of this repository.

param(
    [string]$Out = 'P:\BC-250\scratch\m5psp',
    [string]$Firmware = 'P:\BC-250\ref\linux-firmware__WARN-AMD-blobs-never-commit\amdgpu',
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
$evid = Join-Path $repo 'evidence\linux\2026-09-21-E03-init-trace'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

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

# AMD's interface header psp_gfx_if.h uses a nameless union and bit fields of type uint32_t. Both are
# accepted by every compiler this code meets; MSVC reports them at /W4 (C4201, C4214). Turned off from
# the build line, for the files that include the header, and nowhere else. C4244 and C4701 are the
# ones run.ps1 explains; C4100: psp_v11_0_8_ring_stop() does not use its ring_type argument (there
# is one ring). Imports only.
$pspWarn = @('/wd4201', '/wd4214')
$importWarn = @('/wd4244', '/wd4701', '/wd4100')

$shimSources = @((Join-Path $shim 'shim.c'), (Join-Path $shim 'bc250_gmc.c'), (Join-Path $shim 'bc250_psp.c'))
$importSources = @('gfxhub_v2_0.c', 'mmhub_v2_0.c', 'cyan_skillfish_reg_init.c', 'psp_v11_0_8.c') | ForEach-Object { Join-Path $imports $_ }
$testSources = @((Join-Path $shim 'test\replay_psp.c'))

$incUser = @("/I$shim\include", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, host test)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS') + $pspWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $shimSources + $testSources)
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi') + $pspWarn + $importWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $importSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\replay_psp.exe", "/PDB:$Out\replay_psp.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'compile (kernel flags, same sources, no link)'
$incKern = @("/I$shim\include", "/I$imports", "/I$amdhdr",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um")
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214', '/DBC250_SHIM_KERNEL',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $shimSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $importWarn + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $importSources)

Write-Host 'extract the reference trace'
$traceFile = Join-Path $Out 'trace-psp.txt'
& python (Join-Path $repo 'tools\trace\extract_phase.py') (Join-Path $evid 'amdgpu-events.txt') `
    --match 'MP0\.MP0_SMN_C2PMSG_(6|7)' --reads --until 0.309 | Set-Content -Encoding ascii $traceFile
if ($LASTEXITCODE -ne 0) { throw 'extract_phase.py failed' }

Write-Host 'run'
$argv = @((Join-Path $evid 'sweep-before-run1-GC-complete-then-hang.log'),
          (Join-Path $evid 'sweep-before-run2-nonGC.log'),
          $traceFile, $Firmware)
if ($Verbose250) { $argv += '-v' }
& "$Out\replay_psp.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "replay_psp.exe exit code $code"
exit $code
