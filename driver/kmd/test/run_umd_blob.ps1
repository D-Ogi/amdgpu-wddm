# Host test for driver\kmd\umd_blob.c: the private-blob reader CreateAllocation, CreateContext and
# SubmitCommandVirtual share. The test includes the real contract header; the reader does not.
#
#   pwsh driver\kmd\test\run_umd_blob.ps1
#   pwsh driver\kmd\test\run_umd_blob.ps1 -Out P:\BC-250\scratch\umdblob
#
# The binary goes under -Out. Nothing is written to drive C:.

param(
    [string]$Out = 'P:\BC-250\scratch\umdblob',
    [string]$Kits = 'P:\BC-250\toolchain\nuget',
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$repo = Split-Path (Split-Path $kmd)
$contract = Join-Path $repo 'driver\contract'
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

# /Zp8 is the driver's packing. The contract's own asserts allow any packing, and this is the one
# the KMD is built with, so a field that moved under it fails here rather than on the machine.
$incUser = @("/I$kmd", "/I$contract", "/I$contract\third_party", "/I$contract\uapi-shim",
    "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host, contract header, /Zp8)'
# C4201: nameless unions in the imported amdgpu UAPI, the same warning the contract harness and the
# driver build both silence. C4127: the constant comparisons that pin the reader's numbers to the header.
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/wd4201', '/wd4127', '/Od', '/Zi', '/Zp8') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\umd_blob_test.exe") +
    @((Join-Path $kmd 'umd_blob.c'), (Join-Path $here 'umd_blob_test.c')) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
& "$Out\umd_blob_test.exe"
$code = $LASTEXITCODE

Write-Host 'compile-check (kernel flags, reader only, no contract header, no link)'
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$kmd", "/Fo$obj\kernel-", "/Fd$obj\clkernel.pdb")
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + @((Join-Path $kmd 'umd_blob.c')))

Write-Host ''
Write-Host "umd_blob_test.exe exit code $code"
exit $code
