# Builds and runs the two hardware-monitor host tests, and compile-checks the policy with the kernel flags.
#
#   hwmon_test.c         the policy (driver/shim/bc250_hwmon.c): the access sequence, the read allowlist, the
#                        refusal of every write, the base rules, the conversions, the plausibility rules, the
#                        identity and one sample, against the EC model in hwmon_ec_mock.h.
#   hwmon_native_test.c  the binding (driver/kmd/hwmon.c, compiled here against hwmon_native_mock.h): the
#                        registry gate, the start that never fails, the identity refusals, the sampler's retry
#                        and give-up rules, the published snapshot, the ageing and the escape.
#   fan_test.c           the fan control (driver/shim/bc250_fan.c, Part B): the write allowlist and the handshake
#                        order against the M803 engine model, the restore record, every exit path, doubt, the
#                        slope rule, the emergency, the lease and the chip's refusals.
#
# The second one compiles the shipping file itself, with only the Windows kernel primitives replaced, so it
# tests the driver and not a copy of it. Both assert that the EC model saw no write outside the page and the
# index port of its own window: that is the rule the whole read path rests on.
#
#   pwsh driver\shim\test\run_hwmon.ps1
#   pwsh driver\shim\test\run_hwmon.ps1 -Out P:\BC-250\scratch\build\hwmon
#
# Host-side only: nothing here touches the lab machine, and no port is written anywhere but in the model.
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\hwmon',
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
$objFan = Join-Path $Out 'obj-fan'
New-Item -ItemType Directory -Force $Out, $objPolicy, $objNative, $objKern, $objFan | Out-Null
Remove-Item "$objPolicy\*.obj", "$objNative\*.obj", "$objKern\*.obj", "$objFan\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code|^Generowanie') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$policy = Join-Path $shim 'bc250_hwmon.c'
$incUser = @("/I$shim\include",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

$fan = Join-Path $shim 'bc250_fan.c'

# /wd4505: the model helpers that only one of the tests uses.
Write-Host 'compile (policy, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS', '/wd4505') +
    $incUser + @("/Fo$objPolicy\", "/Fd$objPolicy\cl.pdb", $policy, (Join-Path $here 'hwmon_test.c')))

Write-Host 'compile (fan control, user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS', '/wd4505') +
    $incUser + @("/Fo$objFan\", "/Fd$objFan\cl.pdb", $policy, $fan, (Join-Path $here 'fan_test.c')))

Write-Host 'compile (policy and fan control, kernel flags)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8', '/TC',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DBC250_SHIM_KERNEL', '/wd4201', '/wd4214',
    "/I$shim\include", "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\", $policy, $fan))

# The shipping binding, with its one kernel include swapped for the mock. Nothing else in the file is touched,
# and the generated copy is read back into the compiler so a diff of it is the whole difference under test.
Write-Host 'generate (the binding, with the kernel primitives mocked)'
$source = Get-Content -LiteralPath (Join-Path $kmd 'hwmon.c') -Raw
$needle = '#include "bc250kmd.h"'
if (-not $source.Contains($needle)) { throw "driver\kmd\hwmon.c no longer includes the miniport header: the mock cannot replace it" }
$native = $source.Replace($needle, '#include "hwmon_native_mock.h"')
[IO.File]::WriteAllText((Join-Path $Out 'hwmon-native.inc'), $native, [Text.UTF8Encoding]::new($false))

Write-Host 'compile (binding, user mode)'
# /wd4201 the nameless struct inside D3DDDI_ESCAPEFLAGS, as d3dukmdt.h declares it; /wd4505 the model helpers
# that only one of the two tests uses.
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS',
    '/wd4201', '/wd4505', "/I$Out", "/I$here", "/I$kmd") + $incUser +
    @("/Fo$objNative\", "/Fd$objNative\cl.pdb", $policy, (Join-Path $here 'hwmon_native_test.c')))

$link = @('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
    "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64")
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\hwmon_test.exe", "/PDB:$Out\hwmon_test.pdb") + (Get-ChildItem "$objPolicy\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\hwmon_native_test.exe", "/PDB:$Out\hwmon_native_test.pdb") + (Get-ChildItem "$objNative\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\fan_test.exe", "/PDB:$Out\fan_test.pdb") + (Get-ChildItem "$objFan\*.obj").FullName)

Write-Host 'run'
$code = 0
foreach ($test in 'hwmon_test', 'hwmon_native_test', 'fan_test') {
    & "$Out\$test.exe"
    if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE; break }
}
Write-Host "hardware monitor host tests exit code $code"
exit $code
