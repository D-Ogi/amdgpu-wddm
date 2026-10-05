# Host test for driver\kmd\gdi_admission.h (BD-060): the standard-allocation decision, its request/answer counters
# and the LB7A CreateAllocation/OpenAllocation admission with its rollback accounting, built and run with no WDK
# header. The header is compiled with the kernel flags as part of wddm.c by driver\kmd\build.ps1, which is where its
# interlocked counter is defined.
#
#   pwsh driver\kmd\test\run_gdi_admission.ps1 -Out P:\BC-250\scratch\gdi-admission
#
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\gdi-admission",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
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

$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
$env:INCLUDE = ''
$env:LIB = ''

$incUser = @("/I$kmd", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host, no WDK header)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/Od', '/Zi') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\gdi_admission_test.exe") +
    @((Join-Path $kmd 'dcn_translate.c'), (Join-Path $here 'gdi_admission_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
& "$Out\gdi_admission_test.exe"
$code = $LASTEXITCODE
Write-Host ''
Write-Host "gdi_admission_test.exe exit code $code"
exit $code
