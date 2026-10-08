# Host test of driver\kmd\driver_version.h (the per-application graphics setting ReportAmdDriverVersion: the AMD-scheme
# number, the decision of a start, the Control\Video guard), and a compile check of the same header with the kernel
# flags.
#
#   pwsh driver\kmd\test\run_driver_version.ps1 -Out $env:BC250_ROOT\scratch\driver-version
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\driver-version",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$env:INCLUDE = ''
$env:LIB = ''

$incUser = @("/I$kmd", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/Od', '/Zi') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\driver_version_test.exe") + @((Join-Path $here 'driver_version_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
& "$Out\driver_version_test.exe"
$code = $LASTEXITCODE

Write-Host 'compile-check (kernel flags, the header alone)'
$probe = Join-Path $Out 'driver_version_kernel.c'
Set-Content -Path $probe -Value '#include <ntddk.h>', '#include "driver_version.h"',
    'int DriverVersionProbe(const wchar_t* Path, unsigned long Chars) { DRIVER_VERSION_PLAN plan; wchar_t out[DRIVER_VERSION_PATH_CHARS]; DriverVersionDecide(1, Path, NULL, NULL, &plan); return (int)plan.Action + DriverVersionIsVideoKey(Path, Chars) + DriverVersionIsGuidText(Path, Chars) + DriverVersionIsInstanceName(Path, Chars) + (int)DriverVersionVideoPath(Path, Chars, Path, Chars, out, DRIVER_VERSION_PATH_CHARS) + DriverVersionIsAdapterKey(Path, Chars); }' -Encoding ascii
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$kmd", "/Fo$obj\kernel-") + @($probe))

Write-Host ''
Write-Host "driver_version_test.exe exit code $code"
exit $code
