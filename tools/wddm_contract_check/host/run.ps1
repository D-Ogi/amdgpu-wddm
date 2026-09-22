# Builds and runs the host buffer tests for DxgkDdiQueryAdapterInfo of driver\kmd\wddm.c.
#
#   pwsh tools\wddm_contract_check\host\run.ps1
#   pwsh tools\wddm_contract_check\host\run.ps1 -Kits P:\BC-250\toolchain\nuget -Verbose250
#
# Host-side only: no lab machine, no hardware, no driver load. Everything is written under -Out
# (default P:\BC-250\scratch\build\contract-check), never into the repository and never onto drive C:.
#
# The point of this script is the first compile below: driver\kmd\wddm.c, unmodified, built a second
# time without /kernel and linked into a console program. The kernel services it calls come from
# host_stubs.c, the DDI it is called through comes out of its own WddmBuildTable(). Two compiles, not
# one: the kernel headers and the C runtime cannot share a translation unit, so qai_bridge.c sees
# ntifs.h and no CRT, and qai_test.c sees the CRT and no ntifs.h (qai_bridge.h is the border).

param(
    [string]$Out = 'P:\BC-250\scratch\build\contract-check',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Verbose250,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$kmd = Join-Path $repo 'driver\kmd'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'no Visual Studio build tools found (vswhere returned nothing)' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'    # headers and import libraries come
                                                               # from two different packages
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''

# --- the driver half: wddm.c as the miniport compiles it, minus /kernel ---------------------------
# Same include set, same /Zp8, same warning bar and the same two WDK-header warning disables as
# driver\kmd\build.ps1. /kernel itself is dropped (it is a code-generation switch for a driver image)
# and /GS- stays so that no security cookie from the kernel libraries is needed.
$incKm = @("/I$here", "/I$kmd",
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\um")
$kmFlags = @('/nologo', '/c', '/TC', '/GS-', '/W4', '/WX', '/Od', '/Zi', '/Zp8',
    '/wd4201', '/wd4214',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C',
    '/D_WIN32_WINNT=0x0A00', '/DNDEBUG')

Write-Host 'compile (driver\kmd\wddm.c, unmodified, plus the bridge)'
Invoke-Tool (Join-Path $bin 'cl.exe') ($kmFlags + $incKm + @("/Fo$obj\", "/Fd$obj\cl.pdb",
    (Join-Path $kmd 'wddm.c'), (Join-Path $here 'qai_bridge.c')))

# --- the host half: the stub kernel and the tests --------------------------------------------------
$incUser = @("/I$here", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile (the stub kernel and the tests)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi',
        '/D_CRT_SECURE_NO_WARNINGS') + $incUser + @("/Fo$obj\", "/Fd$obj\cl.pdb",
    (Join-Path $here 'host_stubs.c'), (Join-Path $here 'qai_test.c')))

Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') (@('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
        "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64",
        "/OUT:$Out\qai_test.exe", "/PDB:$Out\qai_test.pdb") + (Get-ChildItem "$obj\*.obj").FullName)

if ($BuildOnly) { Write-Host "built: $Out\qai_test.exe"; exit 0 }

Write-Host 'run'
Write-Host ''
$argv = @()
if ($Verbose250) { $argv += '-v' }
& "$Out\qai_test.exe" @argv
$code = $LASTEXITCODE
Write-Host ''
Write-Host "qai_test.exe exit code $code"
exit $code
