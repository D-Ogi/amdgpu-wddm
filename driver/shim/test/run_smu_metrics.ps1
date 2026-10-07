# Builds and runs the two SMU metrics host tests (KMD 0.7.215, docs/design/dpm.md "Power reading"), and
# compile-checks the policy with the kernel flags.
#
#   smu_metrics_test.c         the policy (driver/shim/bc250_smu_metrics.c): the table's offsets against an independent
#                              declaration of the firmware's structure, the three messages and their one argument each,
#                              the page the driver may name, the decode's refusals and the reader's rules (cadence,
#                              freshness, the refusal latch, the bad-table limit, an offline owner).
#   smu_metrics_native_test.c  the binding (driver/kmd/smu_metrics.c, compiled here against smu_metrics_native_mock.h):
#                              the registry gate, the page and its mapping, one read a second from the governor's
#                              ticks, the latch across starts, the escape's copy, the ageing and the log.
#
# The SMU owner's half (driver/kmd/smu.c SmuReadMetrics: the order of the three messages, the poison, the allowlist
# under the owner lock) is in smu_native_test.c (run_smu_native.ps1).
#
#   pwsh driver\shim\test\run_smu_metrics.ps1
#   pwsh driver\shim\test\run_smu_metrics.ps1 -Out P:\BC-250\scratch\build\smu-metrics
#
# Host-side only: nothing here touches the lab machine. Everything is written under -Out, never into the repository
# and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\smu-metrics',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$kmd = Join-Path $repo 'driver\kmd'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objPolicy = Join-Path $Out 'obj-policy'
$objNative = Join-Path $Out 'obj-native'
$objKern = Join-Path $Out 'obj-kernel'
New-Item -ItemType Directory -Force $Out, $objPolicy, $objNative, $objKern | Out-Null
Remove-Item "$objPolicy\*.obj", "$objNative\*.obj", "$objKern\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code|^Generowanie') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$policy = Join-Path $shim 'bc250_smu_metrics.c'
$incUser = @("/I$shim\include",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (policy, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS') +
    $incUser + @("/Fo$objPolicy\", "/Fd$objPolicy\cl.pdb", $policy, (Join-Path $here 'smu_metrics_test.c')))

Write-Host 'compile (policy, kernel flags)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8', '/TC',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DBC250_SHIM_KERNEL', '/wd4201', '/wd4214',
    "/I$shim\include", "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\", $policy))

# The shipping binding, with its one kernel include swapped for the mock. The VRAM reservation table is copied out of
# driver/kmd/bc250kmd.h line for line, so the page the test expects is the page the driver names.
Write-Host 'generate (the binding, with the kernel primitives mocked)'
$needle = '#include "bc250kmd.h"'
$source = Get-Content -LiteralPath (Join-Path $kmd 'smu_metrics.c') -Raw
if (-not $source.Contains($needle)) { throw "driver\kmd\smu_metrics.c no longer includes the miniport header: the mock cannot replace it" }
[IO.File]::WriteAllText((Join-Path $Out 'smu_metrics-native.inc'), $source.Replace($needle, '#include "smu_metrics_native_mock.h"'),
    [Text.UTF8Encoding]::new($false))
$reservations = @(Get-Content -LiteralPath (Join-Path $kmd 'bc250kmd.h') | Where-Object { $_ -match '^#define BC250_VRAM_\w+\s' })
if ($reservations.Count -lt 5 -or -not ($reservations -match 'BC250_VRAM_SMU_TABLE_BELOW')) {
    throw 'driver\kmd\bc250kmd.h: the VRAM reservation table or BC250_VRAM_SMU_TABLE_BELOW not found'
}
[IO.File]::WriteAllText((Join-Path $Out 'vram_reservations.generated.h'),
    "#pragma once`r`n" + ($reservations -join "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))

Write-Host 'compile (binding, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$Out", "/I$here", "/I$kmd") + $incUser +
    @("/Fo$objNative\", "/Fd$objNative\cl.pdb", $policy, (Join-Path $here 'smu_metrics_native_test.c')))

$link = @('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
    "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64")
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\smu_metrics_test.exe", "/PDB:$Out\smu_metrics_test.pdb") + (Get-ChildItem "$objPolicy\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\smu_metrics_native_test.exe", "/PDB:$Out\smu_metrics_native_test.pdb") + (Get-ChildItem "$objNative\*.obj").FullName)

Write-Host 'run'
$code = 0
foreach ($test in 'smu_metrics_test', 'smu_metrics_native_test') {
    & "$Out\$test.exe"
    if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE; break }
}
Write-Host "SMU metrics host tests exit code $code"
exit $code
