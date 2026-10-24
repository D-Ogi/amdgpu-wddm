# Host test of the escapes admitted while OS paging can run (BD-235). No lab access.
# generate_escape_admission_test.py extracts display.c's own WddmDiagnosticAllowed; escape_admission_test.c runs every
# command 0-1023 with ops 0-15 through it, in the release and in the lab flavour, and fails when a ring writer is
# admitted or when the admitted set differs from the pinned one. Each program runs bounded and windowless and counts only
# with its summary line (host_native.ps1).
#   -Mutation admit-fence  the negative control: RUN_FENCE, a raw SDMA0 writer, is admitted under FullWddm. Each flavour
#                          must then fail by exactly 32 checks (16 ops, two checks each).
# The test is user-mode code: clang-cl 23.1.4 from the workspace toolchain, named by path, with no fallback.
param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Out = "$Root\scratch\escape-admission",
    [string]$Kits = "$Root\toolchain\nuget",
    [string]$KitVersion = '10.0.26100.0',
    [string]$Llvm = "$Root\toolchain\llvm-23.1.4-win64",
    [string]$SourceRoot = '',
    [ValidateSet('none', 'admit-fence')][string]$Mutation = 'none',
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

& python "$here\generate_escape_admission_test.py" "$Out\escape_admission_actual.inc" "$SourceRoot\driver\kmd" $Mutation
if ($LASTEXITCODE -ne 0) { throw 'extraction failed' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdklib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$env:INCLUDE = ''; $env:LIB = ''; $env:CL = $null; $env:_CL_ = $null
. "$here\host_native.ps1"
$results = @{}
foreach ($flavour in 'release', 'lab') {
    $exe = "$Out\escape_admission_$flavour.exe"
    $flags = @('/nologo', '--target=x86_64-pc-windows-msvc', '-fuse-ld=lld', '/TC', '/W4', '/WX', '/Od', '/MT',
        "/I$Out", "/I$repo\driver\kmd", "/imsvc$($msvc.FullName)\include", "/imsvc$sdk\Include\$KitVersion\ucrt",
        "/Fo$Out\$flavour-", "/Fe$exe")
    if ($flavour -eq 'lab') { $flags += '/DAMDGPU_WDDM_LAB_FLAVOUR' }
    & $clang @flags "$here\escape_admission_test.c" '/link' "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$sdklib\ucrt\x64" "/LIBPATH:$sdklib\um\x64"
    if ($LASTEXITCODE -ne 0) { throw "escape admission test ($flavour): compile failed" }
    $results[$flavour] = Invoke-HostTest $exe "$Out\escape_admission_$flavour.log" "^escape admission \($flavour\): \d+ admitted; (?<checks>\d+) checks, (?<failures>\d+) failures$" $TimeoutSeconds
}
if ($Mutation -eq 'none') {
    foreach ($flavour in 'release', 'lab') { if ($results[$flavour].Failures -ne 0) { throw "escape admission test ($flavour) failed" } }
    exit 0
}
foreach ($flavour in 'release', 'lab') {
    if ($results[$flavour].Code -ne 1 -or $results[$flavour].Failures -ne 32) {
        throw "negative control $Mutation ($flavour): $($results[$flavour].Failures) failures, 32 expected"
    }
}
Write-Host "negative control $Mutation : the test failed as required (32 failures in each flavour)"
exit 0
