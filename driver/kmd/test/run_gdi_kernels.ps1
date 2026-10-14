# Host test and build gate of the GDI compute path (HLK gap G1, increment 2 of
# docs\design\gdi-hw-accel.md): driver\kmd\gdi_kernels.cl, the generated code object header and
# driver\kmd\gdi_dispatch.c.
#
# Two parts, in this order:
#   1. tools\gpu\gdi_kernels.py --check. It holds driver\kmd\gdi_kernels.generated.h against the
#      source it records, and where the device compiler is present it builds the kernels again and
#      compares the whole header byte for byte. Without that compiler the two hashes still catch a
#      kernel that was edited and not regenerated.
#   2. the host test: the dispatch emitter's packets are decoded, the kernels' own source is run
#      over the grid they describe, and the result is compared with the reference model byte for
#      byte. Built with no WDK header, then compiled again with the kernel flags the driver uses.
#
#   pwsh driver\kmd\test\run_gdi_kernels.ps1 -Out P:\BC-250\scratch\gdi-hw\out
#
# Everything is written under -Out, never into the repository and never onto drive C:.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\gdi-kernels-test",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path -Parent (Split-Path -Parent $kmd)
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue

$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

Write-Host 'the code object: build gate'
$env:BC250_ROOT = $Root
& python (Join-Path $repo 'tools\gpu\gdi_kernels.py') --check --work (Join-Path $Out 'kernels')
if ($LASTEXITCODE -ne 0) { throw "gdi_kernels.py --check failed ($LASTEXITCODE)" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'

$env:INCLUDE = ''
$env:LIB = ''
$incUser = @("/I$kmd", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host, no WDK header)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/Od', '/Zi') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\gdi_kernel_test.exe") +
    @((Join-Path $kmd 'gdi_render.c'), (Join-Path $kmd 'gdi_dispatch.c'),
      (Join-Path $kmd 'gdi_batch.c'), (Join-Path $kmd 'gdi_emit.c'), (Join-Path $kmd 'gfx_copy.c'),
      (Join-Path $here 'gdi_kernel_run.c'), (Join-Path $here 'gdi_kernel_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
& "$Out\gdi_kernel_test.exe"
$code = $LASTEXITCODE

Write-Host 'compile-check (kernel flags, same source, no link)'
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$kmd", "/Fo$obj\kernel-", "/Fd$obj\clkernel.pdb")
foreach ($one in @('gdi_dispatch.c', 'gdi_batch.c')) {
    Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + @((Join-Path $kmd $one)))
}

Write-Host ''
Write-Host "gdi_kernel_test.exe exit code $code"
exit $code
