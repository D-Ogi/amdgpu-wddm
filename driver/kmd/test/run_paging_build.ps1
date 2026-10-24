# Host test of the BuildPagingBuffer DDI statuses through wddm.c's own code (BD-235). No lab access.
# BC250_ROOT is the workspace root: the environment variable, else the parent directory of this repository.
# generate_paging_build_test.py extracts WddmBuildPagingBufferImpl and Bc250WddmBuildPagingBuffer; paging_build_test.c
# gives them mock builders. Two negative controls (tools\quality\quick.ps1) edit the extracted COPY, and the test must
# then fail by a FAIL line:
#   -Mutation absorb-inner-failure  a failed GPU_PHYSICAL update or FLUSH_TLB falls through to SUCCESS;
#   -Mutation cpu-virtual-ignore    a failed CPU_VIRTUAL update is journalled and reported as SUCCESS.
# The test is user-mode code: clang-cl 23.1.4 from the workspace toolchain, named by path, with no fallback.
# The test runs without a window and without error dialogs, for at most -TimeoutSeconds; on the deadline it is
# stopped and the run fails (host_native.ps1). Its output is kept in paging_build_test.log, and a run counts only with its
# summary line. A negative control must fail by exactly the number of checks pinned for it below.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\pagingbuild",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Llvm = "$Root\toolchain\llvm-23.1.4-win64",
    [string]$SourceRoot = '',
    [ValidateSet('none', 'absorb-inner-failure', 'cpu-virtual-ignore')][string]$Mutation = 'none',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
if (-not $SourceRoot) { $SourceRoot = $repo }
if ($Mutation -ne 'none' -and -not $PSBoundParameters.ContainsKey('Out')) { $Out = "$Out-$Mutation" }
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $Out, $env:TEMP | Out-Null

$clang = Join-Path $Llvm 'bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "clang-cl not found: $clang" }
$clangVersion = @(& $clang --version)
if ($LASTEXITCODE -ne 0 -or $clangVersion[0] -notmatch 'clang version 23\.1\.4\b') { throw "clang-cl 23.1.4 expected: $($clangVersion[0])" }
Write-Host "host compiler: $clang"
Write-Host "  $($clangVersion[0])"

& python "$here\generate_paging_build_test.py" "$Out\paging_build_actual.inc" $SourceRoot $Mutation
if ($LASTEXITCODE -ne 0) { throw 'extraction failed' }
if ($Mutation -ne 'none') { Write-Host "mutation $Mutation (in the extracted copy)" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE = ''; $env:LIB = ''; $env:CL = $null; $env:_CL_ = $null
# The CRT headers are system headers (/imsvc): /W4 /WX applies to the test and to the extracted wddm.c code.
$flags = @('/nologo', '--target=x86_64-pc-windows-msvc', '-fuse-ld=lld', '/TC', '/W4', '/WX', '/Od', '/MT',
    "/I$Out", "/I$repo\driver\kmd", "/imsvc$($msvc.FullName)\include", "/imsvc$sdk\Include\$KitVersion\ucrt",
    "/Fo$Out\", "/Fe$Out\paging_build_test.exe")
& $clang @flags "$here\paging_build_test.c" '/link' "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$sdklib\ucrt\x64" "/LIBPATH:$sdklib\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'paging build test: compile failed' }
. "$here\host_native.ps1"
$r = Invoke-HostTest "$Out\paging_build_test.exe" "$Out\paging_build_test.log" '^BuildPagingBuffer DDI: (?<checks>\d+) checks, (?<failures>\d+) failures$' $TimeoutSeconds
if ($Mutation -eq 'none') {
    if ($r.Failures -ne 0) { throw "paging build test failed: $($r.Failures) failures" }
    exit 0
}
# Each control is pinned to the failures its rule change causes: another count means the control tests something else.
$expected = @{ 'absorb-inner-failure' = 29; 'cpu-virtual-ignore' = 9 }[$Mutation]
if ($r.Code -ne 1 -or $r.Failures -ne $expected) { throw "negative control $Mutation : $($r.Failures) failures, $expected expected" }
Write-Host "negative control $Mutation : the test failed as required ($expected failures)"
exit 0
