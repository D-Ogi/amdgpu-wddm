# Builds and runs the SDMA fault-injection suite: what bc250_sdma.c does when an allocation fails,
# when it succeeds without a CPU mapping, and when an engine hands back a write pointer that is not
# a pointer. Host-side only: nothing here touches the lab machine, and nothing here compares a
# register sequence with a trace - that is run_gfx.ps1's job and this must not change it.
#
#   pwsh driver\shim\test\run_sdma_faults.ps1
#   pwsh driver\shim\test\run_sdma_faults.ps1 -Out P:\BC-250\scratch\build\sdma-faults -Verbose250
#
# Everything is written under -Out (default P:\BC-250\scratch\build\sdma-faults), never into the
# repository and never onto drive C:.
#
# Exit code 0 when every expectation holds. A confirmed defect is written in the test as an expected
# failure with its id, so the suite stays green while the defect stands and says its name on every
# run; the day a defect is fixed its expectation passes, the test reports XPASS and the exit code
# goes non-zero, which is the reminder to come back and turn the marker off.

param(
    [string]$Out = 'P:\BC-250\scratch\build\sdma-faults',
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

# The code under test and the two shim files it needs: the ring write path and the one NBIO register
# the ring setup writes. No backend_mem.c and no backend_trace.c - this suite declares its own
# allocator and its own register file, so that an instrumented allocator can never reach the replay.
#   C4245  as in run_gfx.ps1: AMD's PACKET3()/SDMA packet macros yield a signed int with bit 31 set.
$packetWarn = @('/wd4245')
$packetSources = @('bc250_sdma.c', 'bc250_ring.c') | ForEach-Object { Join-Path $shim $_ }
# shim.c is here for the three amdgpu_sriov_* stubs soc15_common.h's register macros name in the
# branch this part never takes.
$plainSources = @('bc250_nbio.c', 'shim.c') | ForEach-Object { Join-Path $shim $_ }
$testSources = @((Join-Path $shim 'test\sdma_faults.c'))
$importSources = @((Join-Path $imports 'cyan_skillfish_reg_init.c'))
$importWarn = @('/wd4244', '/wd4701')   # the same two the other suites turn off for the imports

$incUser = @("/I$shim\include", "/I$shim", "/I$imports", "/I$amdhdr",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode, fault injection)'
$userFlags = @('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS')
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $packetWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $packetSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $plainSources + $testSources)
Invoke-Tool (Join-Path $bin 'cl.exe') ($userFlags + $importWarn +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $importSources)

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64",
        "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\sdma_faults.exe", "/PDB:$Out\sdma_faults.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)

Write-Host 'run'
$argv = @()
if ($Verbose250) { $argv += '-v' }
& "$Out\sdma_faults.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "sdma_faults.exe exit code $code"
exit $code
