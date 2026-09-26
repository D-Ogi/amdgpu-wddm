# Builds and runs the M7 Stage B page table test - DXGK_PTE to AMD page table entry - and
# compile-checks the same shim source with the WDK kernel flags the miniport uses. Host-side only:
# nothing here touches the lab machine, and the unit under test reads and writes no register at
# all, so there is no trace to extract and no sweep to load.
#
#   pwsh driver\shim\test\run_pte.ps1
#   pwsh driver\shim\test\run_pte.ps1 -Out $env:BC250_ROOT\scratch\m7-pte -Verbose250
#
# Everything is written under -Out (default <BC250_ROOT>\scratch\m7-pte), never into the repository and
# never onto drive C:. BC250_ROOT is the workspace root: the environment variable, else the parent
# directory of this repository.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\m7-pte",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$imports = Join-Path $repo 'driver\amdgpu-import'     # amdgpu_doorbell.h, which amdgpu.h pulls in
$amdhdr = Join-Path $repo 'third_party\linux-amdgpu'  # navi10_enum.h, for the memory types

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

# bc250_gart.c is built in on purpose and not as a convenience: the test compares the flags
# bc250_pte.c produces for the VMID 0 aperture with the ones bc250_gart.c put in unit A's table
# (facts M33 and M37), and that comparison is worth nothing if the two are not the same binary.
$shimSources = @('bc250_pte.c', 'bc250_gart.c') | ForEach-Object { Join-Path $shim $_ }
$testSources = @('test\replay_pte.c') | ForEach-Object { Join-Path $shim $_ }

# The test includes the Windows SDK's own <d3dkmthk.h> to get at the real DXGK_PTE, which is the
# whole point of it; the shared SDK headers come first in the list for that reason. Everything here
# compiles at a clean /W4 /WX with no exceptions, the WDDM headers included.
$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, host test)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $incUser +
    @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $shimSources + $testSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\replay_pte.exe", "/PDB:$Out\replay_pte.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

# The shim half with the flags of driver\kmd\build.ps1. Compile only, and without the test, which
# is user mode by nature. bc250_pte.c has to build here because driver/kmd is where it will be
# called from: the miniport hands it DXGK_PTE::Flags and PageAddress out of a paging buffer.
Write-Host 'compile (kernel flags, the shim sources only, no link)'
$incKern = @("/I$shim\include", "/I$imports", "/I$amdhdr",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\um")
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/wd4201', '/wd4214', '/DBC250_SHIM_KERNEL',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + $incKern + @("/Fo$objKern\", "/Fd$objKern\cl.pdb") + $shimSources)

Write-Host 'run'
$argv = @()
if ($Verbose250) { $argv += '-v' }
& "$Out\replay_pte.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "replay_pte.exe exit code $code"
exit $code
