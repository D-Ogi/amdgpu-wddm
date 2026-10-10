# Builds and runs the two CPU host tests, and compile-checks the policy with the kernel flags.
#
#   cpu_test.c         the policy (driver/shim/bc250_cpu.c): queue 3's three mailbox registers, the two queues'
#                      message allowlists and their argument ranges, the admitted settings, the order a change is
#                      sent in, the signed packing of the curve scale, the failure signs, the guided undervolt
#                      search and the boost sweep's stop rule.
#   cpu_native_test.c  the binding (driver/kmd/cpu.c, compiled here against cpu_native_mock.h): the read stage and
#                      the boost probe's SWEEP, against a firmware model whose replies change between the sweep's
#                      windows. A pure function cannot hold that loop, which is audit finding F3.
#
# The second one compiles the shipping file itself, with only the Windows kernel primitives and the mailbox owner
# replaced, so it tests the driver and not a copy of it.
#
#   pwsh driver\shim\test\run_cpu.ps1
#   pwsh driver\shim\test\run_cpu.ps1 -Out P:\BC-250\scratch\build\cpu
#   pwsh driver\shim\test\run_cpu.ps1 -Source <a copy of cpu.c>   # the negative control's tree
#
# Host-side only: nothing here touches the lab machine, and no mailbox message is sent anywhere but in the model.
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\build\cpu',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [string]$Source = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..\..')
$shim = Join-Path $repo 'driver\shim'
$kmd = Join-Path $repo 'driver\kmd'
$imports = Join-Path $repo 'driver\amdgpu-import'
# -Source names another copy of driver\kmd\cpu.c, which is how the negative control drives the binding as an
# earlier revision had it. Everything else of the tree stays as it is.
if (-not $Source) { $Source = Join-Path $kmd 'cpu.c' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$objUser = Join-Path $Out 'obj-user'
$objKern = Join-Path $Out 'obj-kernel'
$objNative = Join-Path $Out 'obj-native'
New-Item -ItemType Directory -Force $Out, $objUser, $objKern, $objNative | Out-Null
Remove-Item "$objUser\*.obj", "$objKern\*.obj", "$objNative\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|^Generating Code|^Generowanie') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$sources = @('bc250_cpu.c', 'bc250_dpm.c', 'bc250_clock.c') | ForEach-Object { Join-Path $shim $_ }
$incUser = @("/I$shim\include", "/I$imports",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$($msvc.FullName)\include")

Write-Host 'compile (user mode)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS') +
    $incUser + @("/Fo$objUser\", "/Fd$objUser\cl.pdb") + $sources + @((Join-Path $shim 'test\cpu_test.c')))

Write-Host 'compile (kernel flags, bc250_cpu.c, bc250_dpm.c and bc250_clock.c)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zp8', '/TC',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DBC250_SHIM_KERNEL', '/wd4201', '/wd4214',
    "/I$shim\include", "/I$imports", "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt",
    "/I$wdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\shared", "/Fo$objKern\") + $sources)

# The shipping binding, with its one miniport include swapped for the mock. Nothing else in the file is touched,
# and the generated copy is read back into the compiler, so a diff of it is the whole difference under test.
Write-Host 'generate (the binding, with the kernel primitives and the mailbox owner mocked)'
$source = Get-Content -LiteralPath $Source -Raw
$needle = '#include "bc250kmd.h"'
if (-not $source.Contains($needle)) { throw "$Source no longer includes the miniport header: the mock cannot replace it" }
[IO.File]::WriteAllText((Join-Path $Out 'cpu-native.inc'), $source.Replace($needle, '#include "cpu_native_mock.h"'),
    [Text.UTF8Encoding]::new($false))

Write-Host 'compile (binding, user mode)'
# /wd4201 the nameless struct inside D3DDDI_ESCAPEFLAGS, as d3dukmdt.h declares it; /wd4505 the model helpers that
# the arms of this test do not all use.
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/c', '/TC', '/W4', '/WX', '/Od', '/Zi', '/D_CRT_SECURE_NO_WARNINGS',
    '/wd4201', '/wd4505', "/I$Out", "/I$here", "/I$kmd") + $incUser +
    @("/Fo$objNative\", "/Fd$objNative\cl.pdb") + $sources + @((Join-Path $here 'cpu_native_test.c')))

$link = @('/nologo', '/DEBUG', '/MACHINE:X64', '/SUBSYSTEM:CONSOLE',
    "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64")
Write-Host 'link'
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\cpu_test.exe", "/PDB:$Out\cpu_test.pdb") + (Get-ChildItem "$objUser\*.obj").FullName)
Invoke-Tool (Join-Path $bin 'link.exe') ($link +
    @("/OUT:$Out\cpu_native_test.exe", "/PDB:$Out\cpu_native_test.pdb") + (Get-ChildItem "$objNative\*.obj").FullName)

Write-Host 'run'
$code = 0
foreach ($test in 'cpu_test', 'cpu_native_test') {
    & "$Out\$test.exe"
    if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE; break }
}
Write-Host "cpu host tests exit code $code"
exit $code
