# Builds gltest for x64 and for x86 without a WDK or SDK installation: headers and import libraries come
# from the SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual Studio. Same
# flow as tools\win\vkfillcheck\build.ps1, which builds the same pair of architectures for the same
# reason: the OpenGL ICD is registered twice (OpenGLDriverName for x64 processes, OpenGLDriverNameWow for
# the WoW64 ones), and an x64 client cannot say anything about the 32-bit half of a release.
#
# Release settings, by the project default: /O2 /GL with /LTCG, NDEBUG, static CRT (/MT), and /Brepro so
# that two builds of the same source are the same bytes.
#
# The build ends with the checks that need no GPU (--help, two usage errors), then, unless -SkipSelfTest,
# runs --case all on this PC. The client never shows a window unless it is asked to, so the self-test is
# safe on a development PC. A PC without an OpenGL 3+ ICD fails that self-test and says which case failed.
#
#   pwsh tools\win\gltest\build.ps1
#   pwsh tools\win\gltest\build.ps1 -Kits P:\BC-250\toolchain\nuget -Arch x64
#   pwsh tools\win\gltest\build.ps1 -SkipSelfTest

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'both',
    [int]$SelfTestSeconds = 90,
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\gltest' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
if (-not (Test-Path -LiteralPath (Join-Path $sdk "Include\$KitVersion\um\gl\GL.h"))) {
    throw "no OpenGL header in the SDK package: $sdk\Include\$KitVersion\um\gl\GL.h"
}
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force $Out | Out-Null
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$targets = if ($Arch -eq 'both') { @('x64', 'x86') } else { @($Arch) }
$built = @()
foreach ($a in $targets) {
    $cl = Join-Path $msvc.FullName "bin\Hostx64\$a\cl.exe"
    if (-not (Test-Path -LiteralPath $cl)) { throw "no $a compiler: $cl" }
    $sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$a\c"
    if (-not (Test-Path -LiteralPath $sdkLib)) { throw "no $a SDK libraries: $sdkLib" }
    $exe = Join-Path $Out "gltest-$a.exe"

    # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its
    # full hash.
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\gltest-$a-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
        Write-Host "  previous $a artifact retained as retained\gltest-$a-$hash.exe"
    }

    Write-Host "  cl $a"
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @('/nologo', '/W4', '/WX', '/O2', '/GL', '/MT', '/TC', '/std:c11', '/Brepro', '/DNDEBUG',
        '/D_CRT_SECURE_NO_WARNINGS',
        "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
        "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
        "/Fo$Out\gltest-$a.obj", "/Fe$exe", (Join-Path $here 'gltest.c'),
        '/link', '/LTCG', '/Brepro', '/INCREMENTAL:NO',
        "/LIBPATH:$(Join-Path $msvc.FullName "lib\$a")", "/LIBPATH:$sdkLib\ucrt\$a", "/LIBPATH:$sdkLib\um\$a",
        'opengl32.lib', 'gdi32.lib', 'user32.lib', 'psapi.lib', 'kernel32.lib') |
        ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|generating code|generowanie kodu') { Write-Host "    $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $a ($LASTEXITCODE)" }
    $built += $exe
}

# The image of each artifact must be the architecture it was asked for: an x64 program says nothing about
# the WoW64 half of the ICD registration, which is the whole reason both are built.
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
foreach ($exe in $built) {
    $want = if ($exe -match 'x86\.exe$') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
    $hdr = & $dumpbin /nologo /headers $exe | Select-String 'machine \('
    if ("$hdr" -notmatch $want) { throw "$exe is not the expected image: $hdr" }
    Write-Host "  image $(Split-Path -Leaf $exe): $(($hdr -join ' ').Trim())"
}

foreach ($exe in $built) {
    & $exe --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "help check failed for $exe ($LASTEXITCODE)" }
    & $exe --bad-option 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check failed for $exe ($LASTEXITCODE)" }
    & $exe --case nosuchcase 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "case check failed for $exe ($LASTEXITCODE)" }
    & $exe --seconds 0 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "range check failed for $exe ($LASTEXITCODE)" }
}
Write-Host '  help, option, case and range checks pass'

if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    foreach ($exe in $built) {
        $name = Split-Path -Leaf $exe
        Write-Host "  self-test $name : --case all --seconds $SelfTestSeconds"
        & $exe --case all --seconds $SelfTestSeconds | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "self-test failed for $name ($LASTEXITCODE)" }
        # A comparison that cannot fail proves nothing: with the expected values displaced by one, each
        # comparing case must come back FAIL.
        foreach ($c in @('draw', 'threads', 'compute')) {
            & $exe --case $c --negative-control --seconds $SelfTestSeconds |
                Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
            if ($LASTEXITCODE -ne 1) {
                throw "negative control did not fail for $name --case $c ($LASTEXITCODE)"
            }
        }
        Write-Host "  negative control $name : draw, threads and compute all failed as required"
    }
}

Get-ChildItem $Out -Filter 'gltest-*.exe' | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
