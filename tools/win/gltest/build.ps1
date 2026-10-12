# Builds gltest and glprobe for x64 and for x86 without a WDK or SDK installation: headers and import
# libraries come from the SDK NuGet packages unpacked under -Kits, the compiler from the installed Visual
# Studio. Same flow as tools\win\vkfillcheck\build.ps1, which builds the same pair of architectures for the
# same reason: the OpenGL ICD is registered twice (OpenGLDriverName for x64 processes, OpenGLDriverNameWow
# for the WoW64 ones), and an x64 client cannot say anything about the 32-bit half of a release.
#
# Release settings, by the project default: /O2 /GL with /LTCG, NDEBUG, static CRT (/MT), and /Brepro so
# that two builds of the same source are the same bytes.
#
# The build ends with the checks that need no GPU (--help, the usage errors), then, unless -SkipSelfTest,
# runs gltest --case all on this PC and glprobe against this PC's own OpenGL driver. Neither tool shows a
# window unless it is asked to, so the self-test is safe on a development PC. A PC without an OpenGL 3+ ICD
# fails the gltest self-test and says which case failed; a PC whose display adapter names no ICD fails the
# glprobe self-test, which needs a working ICD as its positive control.
#
#   pwsh tools\win\gltest\build.ps1
#   pwsh tools\win\gltest\build.ps1 -Kits P:\BC-250\toolchain\nuget -Arch x64
#   pwsh tools\win\gltest\build.ps1 -SkipSelfTest
#   pwsh tools\win\gltest\build.ps1 -Tool glprobe

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0',
    [ValidateSet('x64', 'x86', 'both')][string]$Arch = 'both',
    [ValidateSet('gltest', 'glprobe', 'both')][string]$Tool = 'both',
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
$tools = if ($Tool -eq 'both') { @('gltest', 'glprobe') } else { @($Tool) }
$built = @()
foreach ($t in $tools) {
    foreach ($a in $targets) {
        $cl = Join-Path $msvc.FullName "bin\Hostx64\$a\cl.exe"
        if (-not (Test-Path -LiteralPath $cl)) { throw "no $a compiler: $cl" }
        $sdkLib = Join-Path $Kits "microsoft.windows.sdk.cpp.$a\c"
        if (-not (Test-Path -LiteralPath $sdkLib)) { throw "no $a SDK libraries: $sdkLib" }
        $exe = Join-Path $Out "$t-$a.exe"

        # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its
        # full hash.
        if (Test-Path -LiteralPath $exe) {
            $hash = (Get-FileHash -LiteralPath $exe).Hash
            $keep = Join-Path $Out "retained\$t-$a-$hash.exe"
            if (-not (Test-Path -LiteralPath $keep)) {
                New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
                Copy-Item -LiteralPath $exe -Destination $keep
            }
            Write-Host "  previous $t $a artifact retained as retained\$t-$a-$hash.exe"
        }

        Write-Host "  cl $t $a"
        $env:INCLUDE = ''; $env:LIB = ''
        & $cl @('/nologo', '/W4', '/WX', '/O2', '/GL', '/MT', '/TC', '/std:c11', '/Brepro', '/DNDEBUG',
            '/D_CRT_SECURE_NO_WARNINGS',
            "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
            "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
            "/Fo$Out\$t-$a.obj", "/Fe$exe", (Join-Path $here "$t.c"),
            '/link', '/LTCG', '/Brepro', '/INCREMENTAL:NO',
            "/LIBPATH:$(Join-Path $msvc.FullName "lib\$a")", "/LIBPATH:$sdkLib\ucrt\$a", "/LIBPATH:$sdkLib\um\$a",
            'opengl32.lib', 'gdi32.lib', 'user32.lib', 'psapi.lib', 'kernel32.lib') |
            ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$|generating code|generowanie kodu') { Write-Host "    $_" } }
        if ($LASTEXITCODE -ne 0) { throw "cl failed for $t $a ($LASTEXITCODE)" }
        $built += $exe
    }
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
    & $exe --seconds 0 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "range check failed for $exe ($LASTEXITCODE)" }
    & $exe --expect-icd 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "expect-icd value check failed for $exe ($LASTEXITCODE)" }
    if ((Split-Path -Leaf $exe) -like 'gltest-*') {
        & $exe --case nosuchcase 2>$null | Out-Null
        if ($LASTEXITCODE -ne 2) { throw "case check failed for $exe ($LASTEXITCODE)" }
    }
}
Write-Host '  help, option, case and range checks pass'

if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    foreach ($exe in @($built | Where-Object { (Split-Path -Leaf $_) -like 'gltest-*' })) {
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
        # --expect-icd, both ways round, against whatever ICD serves this computer: the module the client itself
        # named has to pass, and a name no module carries has to fail. This is the check a lab arm leans on, because
        # the arm has to prove that OUR registered ICD answered and not the one that was there before.
        $infoText = @(& $exe --case info --seconds $SelfTestSeconds)
        if ($LASTEXITCODE -ne 0) { throw "info run failed for $name ($LASTEXITCODE)" }
        $icdLine = @($infoText | Where-Object { $_ -match 'info: icd module = ' })
        if (-not $icdLine.Count) { throw "no 'icd module' line in the info output of $name" }
        $icdPath = ($icdLine[0] -replace '^.*info: icd module = ', '') -replace ' \(owns.*$', ''
        $icdLeaf = Split-Path -Leaf $icdPath
        & $exe --case info --expect-icd $icdLeaf --seconds $SelfTestSeconds | Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "--expect-icd $icdLeaf did not pass for $name ($LASTEXITCODE)" }
        & $exe --case info --expect-icd 'no_such_icd_module' --seconds $SelfTestSeconds | Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 1) { throw "--expect-icd no_such_icd_module did not fail for $name ($LASTEXITCODE)" }
        Write-Host "  expect-icd $name : passes on $icdLeaf, fails on a name no module carries"
        # Below GL 4.3 the two cases that need compute shaders have to FAIL, not pass with a note: the shared buffer
        # across the fence hand-over is the only cross-context synchronisation the threads case measures, so a run
        # that skipped it says nothing about the driver. --max-version makes that visible on a 4.6 driver.
        foreach ($c in @('compute', 'threads')) {
            & $exe --case $c --max-version 4.2 --seconds $SelfTestSeconds | Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
            if ($LASTEXITCODE -ne 1) { throw "--case $c --max-version 4.2 did not fail for $name ($LASTEXITCODE)" }
        }
        & $exe --case draw --max-version 3.3 --seconds $SelfTestSeconds | Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "--case draw --max-version 3.3 did not pass for $name ($LASTEXITCODE)" }
        Write-Host "  version ladder $name : compute and threads fail below 4.3, draw passes on a core 3.3 context"
    }
    # glprobe against this PC's own OpenGL driver, which is the positive control of every step the lab reads: the
    # kernel must name an ICD for the window's adapter, opengl32 must be seen loading that very file, a module that
    # exports DrvSetContext must carry its name, and the context must not be the generic one. Then the negative
    # control: a name no driver carries must come back unnamed and unloaded, so a "yes" in the lab output can only
    # come from the file it names.
    foreach ($exe in @($built | Where-Object { (Split-Path -Leaf $_) -like 'glprobe-*' })) {
        $name = Split-Path -Leaf $exe
        Write-Host "  self-test $name : this PC's own ICD as the positive control"
        $text = @(& $exe --seconds 30)
        if ($LASTEXITCODE -ne 0) { throw "glprobe run failed for $name ($LASTEXITCODE)" }
        $text | Where-Object { $_ -match 'umopenglinfo|summary|chosen=|wgl create|dll-load .*phase=(pixelformat|context)|direct-load' } |
            ForEach-Object { Write-Host "    $_" }
        if (-not ($text | Where-Object { $_ -match '^glprobe: debug-string phase=start text="glprobe debug-string control"' })) {
            throw "$name : the debug-string watch did not see the probe's own control line"
        }
        if (-not ($text | Where-Object { $_ -match '^glprobe: dll-watch ntstatus=0x00000000 on ' })) {
            throw "$name : the DLL load watch is not on"
        }
        $icd = @($text | Where-Object { $_ -match '^glprobe: summary window-icd="([^"]+)"' } | ForEach-Object { $Matches[1] })
        if (-not $icd.Count) { throw "$name : the kernel named no ICD for the window's adapter on this PC" }
        $leaf = Split-Path -Leaf $icd[0]
        $pos = @(& $exe --seconds 30 --expect-icd $leaf --no-load-icd)
        $want = @("kernel-names-it-for-window=yes", 'opengl32-loaded-it=[1-9]', 'drv-module-carries-it=[1-9]')
        foreach ($w in $want) {
            if (-not ($pos | Where-Object { $_ -match "^glprobe: summary expect=.*$w" })) { throw "$name --expect-icd $leaf : no '$w'" }
        }
        if (-not ($pos | Where-Object { $_ -match '^glprobe: summary pfd-icd=[1-9]\d* chosen-kind=icd context=icd ' })) {
            throw "$name : this PC's ICD did not give an ICD pixel format and an ICD context"
        }
        $neg = @(& $exe --seconds 30 --expect-icd 'no_such_icd_module' --no-load-icd)
        if (-not ($neg | Where-Object { $_ -match '^glprobe: summary expect=.*kernel-names-it-for-window=no .*opengl32-loaded-it=0 .*drv-module-carries-it=0' })) {
            throw "$name --expect-icd no_such_icd_module : the negative control did not come back unnamed and unloaded"
        }
        Write-Host "  glprobe $name : names, loads and finds $leaf; a name no driver carries is neither named nor loaded"
    }
}

Get-ChildItem $Out -Filter '*.exe' | Where-Object { $_.Name -match '^(gltest|glprobe)-(x64|x86)\.exe$' } | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
