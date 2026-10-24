# Host test for the BuildPagingBuffer status rules (BD-235). No lab access.
# BC250_ROOT is the workspace root: the environment variable, else the parent directory of this repository.
# Two negative controls (tools\quality\quick.ps1). Each makes a COPY of the header, and the test must then fail by a
# FAIL line:
#   -Mutation refusal-success  a failure outside the DDI goes to the OS as STATUS_SUCCESS (the rejected BD-235 rule);
#   -Mutation pending-success  a nonnegative status such as STATUS_PENDING goes to the OS as STATUS_SUCCESS.
# Compilers: the host test is user-mode code and is built with clang-cl 23.1.4 from the workspace toolchain, named by
# path, with no fallback. The kernel compile-check of the header uses the WDK's MSVC with the KMD's own flags,
# because that is the compiler that builds bc250kmd.sys (driver\kmd\build.ps1); the KMD compiler is evaluated apart
# from the user-mode policy.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\pagingddistatus",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Llvm = "$Root\toolchain\llvm-23.1.4-win64",
    [ValidateSet('none', 'refusal-success', 'pending-success')][string]$Mutation = 'none'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kmd = Split-Path -Parent $here
$wdk = Join-Path $Kits 'microsoft.windows.wdk.x64\c'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$clang = Join-Path $Llvm 'bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "clang-cl not found: $clang" }
$clangVersion = @(& $clang --version)
$clangExit = $LASTEXITCODE
$clangVersion = $clangVersion[0]
if ($clangExit -ne 0 -or $clangVersion -notmatch 'clang version 23\.1\.4\b') { throw "clang-cl 23.1.4 expected: $clangVersion" }
Write-Host "host compiler: $clang"
Write-Host "  $clangVersion"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'

if ($Mutation -ne 'none' -and -not $PSBoundParameters.ContainsKey('Out')) { $Out = "$Out-$Mutation" }
$test = Join-Path $here 'paging_ddi_status_test.c'
if ($Mutation -ne 'none') {
    # The test includes ../paging_ddi_status.h: the copies keep that layout, the tree stays unchanged.
    $copy = Join-Path $Out 'mutation'
    New-Item -ItemType Directory -Force (Join-Path $copy 'test') | Out-Null
    $header = Get-Content -LiteralPath (Join-Path $kmd 'paging_ddi_status.h') -Raw
    $nl = if ($header.Contains("`r`n")) { "`r`n" } else { "`n" }
    if ($Mutation -eq 'refusal-success') {
        $anchor = "    if (Status >= 0) return PAGING_DDI_NOT_BUILT;$nl    return Status;"
        $replacement = "    if (Status >= 0) return PAGING_DDI_NOT_BUILT;$nl    return PAGING_DDI_SUCCESS;"
    } else {
        $anchor = '    if (Status >= 0) return PAGING_DDI_NOT_BUILT;'
        $replacement = '    if (Status >= 0) return PAGING_DDI_SUCCESS;'
    }
    $hits = $header.Split(@($anchor), [StringSplitOptions]::None).Count - 1
    if ($hits -ne 1) { throw "negative control $Mutation : its lines matched $hits times, once expected" }
    $header = $header.Replace($anchor, $replacement)
    [IO.File]::WriteAllText((Join-Path $copy 'paging_ddi_status.h'), $header)
    Copy-Item -LiteralPath $test -Destination (Join-Path $copy 'test')
    $test = Join-Path $copy 'test\paging_ddi_status_test.c'
    $kmd = $copy
    Write-Host "mutation $Mutation : $($replacement.Trim())"
}

$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $Out, $obj | Out-Null
Remove-Item "$obj\*.obj" -Force -ErrorAction SilentlyContinue

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path -Leaf $exe) failed ($LASTEXITCODE)" }
}

$env:INCLUDE = ''
$env:LIB = ''
$env:CL = $null
$env:_CL_ = $null

# The SDK and CRT headers are system headers (/imsvc): /W4 /WX applies to the test and paging_ddi_status.h.
$incHost = @("/I$kmd", "/imsvc$sdk\Include\$KitVersion\ucrt", "/imsvc$sdk\Include\$KitVersion\um",
    "/imsvc$sdk\Include\$KitVersion\shared", "/imsvc$($msvc.FullName)\include")

Write-Host 'compile and link (host, clang-cl, no WDK header)'
Invoke-Tool $clang (@('/nologo', '--target=x86_64-pc-windows-msvc', '/TC', '/W4', '/WX', '/Od', '/Zi', '/MT',
    '-fuse-ld=lld') + $incHost +
    @("/Fo$obj\", "/Fe$Out\paging_ddi_status_test.exe") +
    @($test) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
$lines = @(& "$Out\paging_ddi_status_test.exe" 2>&1 | ForEach-Object { "$_" })
$code = $LASTEXITCODE
$lines | Write-Host

# The kernel compile-check of the header: the same function against the WDK status values, with the KMD's compiler,
# so that a change of either side fails here and not on the lab.
Write-Host 'compile-check (WDK MSVC, kernel flags, the header against ntstatus.h)'
$probe = Join-Path $Out 'paging_ddi_status_kernel.c'
[IO.File]::WriteAllText($probe, @'
#include <ntddk.h>
#include "paging_ddi_status.h"
C_ASSERT(PAGING_DDI_SUCCESS == STATUS_SUCCESS);
C_ASSERT(PAGING_DDI_INSUFFICIENT_DMA_BUFFER == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
C_ASSERT(PAGING_DDI_ALLOCATION_BUSY == STATUS_GRAPHICS_ALLOCATION_BUSY);
C_ASSERT(PAGING_DDI_NOT_BUILT == STATUS_UNSUCCESSFUL);
NTSTATUS PagingDdiStatusProbe(NTSTATUS Status) { int outside; return (NTSTATUS)PagingDdiStatus(Status, &outside); }
'@)
$kernFlags = @('/nologo', '/c', '/TC', '/kernel', '/GS-', '/W4', '/WX', '/O2', '/Zi', '/Zp8', '/GF', '/Gy',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DWINNT=1', '/DNTDDI_VERSION=0x0A00000C', '/D_WIN32_WINNT=0x0A00', '/DNDEBUG',
    "/I$wdk\Include\$KitVersion\km", "/I$wdk\Include\$KitVersion\km\crt", "/I$wdk\Include\$KitVersion\shared",
    "/I$sdk\Include\$KitVersion\shared", "/I$kmd", "/Fo$obj\kernel-", "/Fd$obj\clkernel.pdb")
Invoke-Tool (Join-Path $bin 'cl.exe') ($kernFlags + @($probe))

Write-Host ''
Write-Host "paging_ddi_status_test.exe exit code $code"
if ($Mutation -eq 'none') { exit $code }
if ($code -ne 1 -or -not ($lines -match '^FAIL line')) { throw "negative control $Mutation did not fail the test" }
Write-Host "negative control $Mutation : the test failed as required"
exit 0
