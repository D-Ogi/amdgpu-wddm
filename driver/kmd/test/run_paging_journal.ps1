# Host test of the paging journal's fence and sequence stamps through paging_journal.c's own code (BD-235 Q3a). No lab
# access. BC250_ROOT is the workspace root: the environment variable, else the parent directory of this repository.
# generate_paging_journal_test.py extracts paging_journal.c without its #include lines; paging_journal_test.c gives it
# the driver's record headers and mock kernel calls. One negative control (tools\quality\quick.ps1) edits the
# extracted COPY, and the test must then fail by a FAIL line:
#   -Mutation stamp-failed  the fence and sequence stamps take the record of a failed build.
# The test is user-mode code: clang-cl 23.1.4 from the workspace toolchain, named by path, with no fallback.
# The test runs without a window and without error dialogs, for at most -TimeoutSeconds; on the deadline it is
# stopped and the run fails (host_native.ps1). Its output is kept in paging_journal_test.log, and a run counts only
# with its summary line. The negative control must fail by exactly the number of checks pinned for it below.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\pagingjournal",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Llvm = "$Root\toolchain\llvm-23.1.4-win64",
    [string]$SourceRoot = '',
    [ValidateSet('none', 'stamp-failed')][string]$Mutation = 'none',
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

& python "$here\generate_paging_journal_test.py" "$Out\paging_journal_actual.inc" $SourceRoot $Mutation
if ($LASTEXITCODE -ne 0) { throw 'extraction failed' }
if ($Mutation -ne 'none') { Write-Host "mutation $Mutation (in the extracted copy)" }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE = ''; $env:LIB = ''; $env:CL = $null; $env:_CL_ = $null
# The CRT headers are system headers (/imsvc): /W4 /WX applies to the test and to the extracted paging_journal.c code.
$flags = @('/nologo', '--target=x86_64-pc-windows-msvc', '-fuse-ld=lld', '/TC', '/W4', '/WX', '/Od', '/MT',
    "/I$Out", "/I$repo\driver\kmd", "/imsvc$($msvc.FullName)\include", "/imsvc$sdk\Include\$KitVersion\ucrt",
    "/Fo$Out\", "/Fe$Out\paging_journal_test.exe")
& $clang @flags "$here\paging_journal_test.c" '/link' "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$sdklib\ucrt\x64" "/LIBPATH:$sdklib\um\x64"
if ($LASTEXITCODE -ne 0) { throw 'paging journal test: compile failed' }
. "$here\host_native.ps1"
$r = Invoke-HostTest "$Out\paging_journal_test.exe" "$Out\paging_journal_test.log" '^paging journal: (?<checks>\d+) checks, (?<failures>\d+) failures$' $TimeoutSeconds
if ($Mutation -eq 'none') {
    if ($r.Failures -ne 0) { throw "paging journal test failed: $($r.Failures) failures" }
    exit 0
}
# Each control is pinned to the failures its rule change causes: another count means the control tests something else.
$expected = @{ 'stamp-failed' = 7 }[$Mutation]
if ($r.Code -ne 1 -or $r.Failures -ne $expected) { throw "negative control $Mutation : $($r.Failures) failures, $expected expected" }
Write-Host "negative control $Mutation : the test failed as required ($expected failures)"
exit 0
