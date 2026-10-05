# Host test for driver\kmd\umd_caps.c: the bytes QueryAdapterInfo will hand a UMD.
#
#   pwsh driver\kmd\test\run_umd_caps.ps1
#   pwsh driver\kmd\test\run_umd_caps.ps1 -Generate
#
# -Generate rewrites driver\kmd\umd_caps.c from the unit A filler, then runs the compare.
# The binary goes under -Out. Nothing is written to drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\umdcaps',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0',
    [switch]$Generate
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path (Split-Path $kmd)
$contract = Join-Path $repo 'driver\contract'
$caps = Join-Path $contract 'test'
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

$env:INCLUDE = ''
$env:LIB = ''

$inc = @("/I$kmd", "/I$contract", "/I$contract\third_party", "/I$contract\uapi-shim", "/I$caps",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")
$cflags = @('/nologo', '/TC', '/W4', '/WX', '/wd4201', '/wd4127', '/Od', '/Zi', '/Zp8', '/D_CRT_SECURE_NO_WARNINGS')

if ($Generate) {
    Write-Host 'generate umd_caps.c from the unit A filler'
    Invoke-Tool (Join-Path $bin 'cl.exe') ($cflags + $inc +
        @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\gen_umd_caps.exe") +
        @((Join-Path $caps 'bc250_caps_unitA.c'), (Join-Path $here 'gen_umd_caps.c')) +
        @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))
    & "$Out\gen_umd_caps.exe" (Join-Path $kmd 'umd_caps.c')
    if ($LASTEXITCODE -ne 0) { throw "gen_umd_caps.exe failed ($LASTEXITCODE)" }
    Remove-Item "$obj\*.obj" -Force
}

Write-Host 'compile and link (compare baked bytes to the filler)'
Invoke-Tool (Join-Path $bin 'cl.exe') ($cflags + $inc +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\umd_caps_test.exe") +
    @((Join-Path $kmd 'umd_caps.c'), (Join-Path $caps 'bc250_caps_unitA.c'), (Join-Path $here 'umd_caps_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
& "$Out\umd_caps_test.exe"
$code = $LASTEXITCODE

Write-Host 'compile-check (kernel flags, baked bytes only, no filler, no link)'
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$kmd", "/Fo$obj\kernel-", "/Fd$obj\clkernel.pdb")
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + @((Join-Path $kmd 'umd_caps.c')))

Write-Host ''
Write-Host "umd_caps_test.exe exit code $code"
exit $code
