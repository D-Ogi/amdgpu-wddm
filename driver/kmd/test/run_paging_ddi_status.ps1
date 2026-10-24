# Host test for the BuildPagingBuffer status contract (BD-235). No lab access.
# BC250_ROOT is the workspace root: the environment variable, else the parent directory of this repository.
# -Mutation refusal-passthrough is the negative control (tools\quality\quick.ps1): a COPY of the header returns
# the refused status to the OS again, as before BD-235, and the test must then fail by a FAIL line.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\pagingddistatus",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('none', 'refusal-passthrough')][string]$Mutation = 'none'
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

if ($Mutation -ne 'none' -and -not $PSBoundParameters.ContainsKey('Out')) { $Out = "$Out-$Mutation" }
$test = Join-Path $here 'paging_ddi_status_test.c'
if ($Mutation -eq 'refusal-passthrough') {
    # The test includes ../paging_ddi_status.h: the copies keep that layout, the tree stays unchanged.
    $copy = Join-Path $Out 'mutation'
    New-Item -ItemType Directory -Force (Join-Path $copy 'test') | Out-Null
    $header = Get-Content -LiteralPath (Join-Path $kmd 'paging_ddi_status.h') -Raw
    $anchor = "    *Refused = 1;`n    return PAGING_DDI_SUCCESS;"
    $hits = $header.Split(@($anchor), [StringSplitOptions]::None).Count - 1
    if ($hits -ne 1) { throw "negative control $Mutation : its lines matched $hits times, once expected" }
    $header = $header.Replace($anchor, "    *Refused = 1;`n    return Status;")
    [IO.File]::WriteAllText((Join-Path $copy 'paging_ddi_status.h'), $header)
    Copy-Item -LiteralPath $test -Destination (Join-Path $copy 'test')
    $test = Join-Path $copy 'test\paging_ddi_status_test.c'
    $kmd = $copy
    Write-Host "mutation $Mutation : a refused status reaches the OS unchanged"
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

$incUser = @("/I$kmd", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$($msvc.FullName)\include")

Write-Host 'compile and link (host, no WDK header)'
Invoke-Tool (Join-Path $bin 'cl.exe') (@('/nologo', '/TC', '/W4', '/WX', '/Od', '/Zi') + $incUser +
    @("/Fo$obj\", "/Fd$obj\cl.pdb", "/Fe$Out\paging_ddi_status_test.exe") +
    @($test) +
    @("/link", "/LIBPATH:$sdklib\ucrt\x64", "/LIBPATH:$sdklib\um\x64", "/LIBPATH:$($msvc.FullName)\lib\x64"))

Write-Host 'run'
$lines = @(& "$Out\paging_ddi_status_test.exe" 2>&1 | ForEach-Object { "$_" })
$code = $LASTEXITCODE
$lines | Write-Host

# The kernel compile-check of the header: the same function against the WDK status values, so that a change of
# either side fails here and not on the lab.
Write-Host 'compile-check (kernel flags, the header against ntstatus.h)'
$probe = Join-Path $Out 'paging_ddi_status_kernel.c'
[IO.File]::WriteAllText($probe, @'
#include <ntddk.h>
#include "paging_ddi_status.h"
C_ASSERT(PAGING_DDI_SUCCESS == STATUS_SUCCESS);
C_ASSERT(PAGING_DDI_INSUFFICIENT_DMA_BUFFER == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
C_ASSERT(PAGING_DDI_ALLOCATION_BUSY == STATUS_GRAPHICS_ALLOCATION_BUSY);
NTSTATUS PagingDdiStatusProbe(NTSTATUS Status) { int refused; return (NTSTATUS)PagingDdiStatus(Status, &refused); }
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
