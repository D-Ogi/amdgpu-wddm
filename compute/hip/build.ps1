# Builds the bc250hsa static library, its five host tests and the step-1 tool hipprobe, without a
# WDK or SDK installation: the headers and the import libraries come from the SDK NuGet packages
# under -Kits and the compiler from the installed Visual Studio, as tools\win\monfence\build.ps1 and
# tools\win\kmtprobe\build.ps1 do. Then it runs everything that can run on the development PC: the
# restate-and-compare gates, the host tests, hipprobe --selftest and --help, and a parse of
# tools\run-lab.ps1. It prints the size and the SHA256 of every artifact.
#
#   pwsh compute\hip\build.ps1
#   pwsh compute\hip\build.ps1 -Kits P:\bc-250\toolchain\nuget -Out P:\bc-250\scratch\build\bc250hsa
#   pwsh compute\hip\build.ps1 -Rebuild      # also rebuilds the test code objects with clang
#   pwsh compute\hip\build.ps1 -CheckDoc     # also compares the header with the design document
#
# Layer 1 of docs\design\m16-hip-route-b.md. Layer 2 has its own script, build-runtime.ps1.

# $Root is the workspace that holds toolchain\nuget and scratch. The repository is checked
# out both as itself and as worktrees under scratch, so the depth from this script to the
# workspace is not fixed: the default walks up until it finds the toolchain.
param(
    [string]$Root = $(
        if ($env:BC250_ROOT) { $env:BC250_ROOT } else {
            $probe = $PSScriptRoot
            while ($probe -and -not (Test-Path (Join-Path $probe 'toolchain\nuget'))) {
                $probe = Split-Path -Parent $probe
            }
            if ($probe) { $probe } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }
        }
    ),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0',
    [string]$Clang = '',
    [switch]$Rebuild,
    [switch]$CheckDoc,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\bc250hsa' }
if (-not $Clang) { $Clang = Join-Path $Root 'toolchain\llvm-amdgpu-22.1.8\mingw64\bin' }

$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'no Visual Studio toolset found (vswhere returned nothing)' }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$lib = Join-Path $msvc.FullName 'bin\Hostx64\x64\lib.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$srcDir = Join-Path $here 'bc250hsa'
$testDir = Join-Path $here 'tests\host'
$dataDir = Join-Path $here 'tests\data'
$toolDir = Join-Path $here 'tools'

# ---------------------------------------------------------------------------------------------
# Gate 1: the frozen header and the design document must hold the same text.
# ---------------------------------------------------------------------------------------------
if ($CheckDoc) {
    $design = Join-Path $repo 'docs\design\m16-hip-route-b.md'
    $headerPath = Join-Path $here 'include\bc250hsa.h'
    $lines = Get-Content -LiteralPath $design
    $start = -1; $stop = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($start -lt 0 -and $lines[$i] -eq '```c' -and $lines[$i + 1] -like '/* bc250hsa.h*') { $start = $i + 1; continue }
        if ($start -ge 0 -and $lines[$i] -eq '```') { $stop = $i - 1; break }
    }
    if ($start -lt 0 -or $stop -lt $start) { throw 'the header block of the design document was not found' }
    $fromDoc = ($lines[$start..$stop] -join "`n").TrimEnd()
    $fromFile = ((Get-Content -LiteralPath $headerPath) -join "`n").TrimEnd()
    if ($fromDoc -ne $fromFile) {
        $a = Join-Path $Out 'header-from-doc.txt'; $b = Join-Path $Out 'header-from-file.txt'
        Set-Content -LiteralPath $a -Value $fromDoc -Encoding utf8
        Set-Content -LiteralPath $b -Value $fromFile -Encoding utf8
        throw "include\bc250hsa.h and the design document differ; see $a and $b"
    }
    Write-Host '  bc250hsa.h equals section 3.9 of the design document'
}

# ---------------------------------------------------------------------------------------------
# Gate 2: constants restated outside their source (repo rules 1 and 2). The PM4 packet numbers,
# the register offsets and the event types are checked in C by tests\host\test_pm4.c, which
# includes the vendored Linux headers themselves. Here are the private blob values, whose source
# header cannot be included by a /std:c11 user-mode build without the Linux user API shim.
# ---------------------------------------------------------------------------------------------
function Assert-Same($SourcePath, $SourcePattern, $ToolPath, $ToolPattern, $Name) {
    foreach ($p in @($SourcePath, $ToolPath)) { if (-not (Test-Path $p)) { throw "$p not found ($Name check)" } }
    $s = Select-String -Path $SourcePath -Pattern $SourcePattern | Select-Object -First 1
    $t = Select-String -Path $ToolPath -Pattern $ToolPattern | Select-Object -First 1
    if ($null -eq $s) { throw "$Name not found in $SourcePath - renamed?" }
    if ($null -eq $t) { throw "$Name not found in $ToolPath" }
    $a = $s.Matches[0].Groups[1].Value -replace 'u$', ''
    $b = $t.Matches[0].Groups[1].Value -replace 'u$', ''
    $va = if ($a -match '^0x') { [Convert]::ToUInt64($a.Substring(2), 16) } else { [UInt64]$a }
    $vb = if ($b -match '^0x') { [Convert]::ToUInt64($b.Substring(2), 16) } else { [UInt64]$b }
    if ($va -ne $vb) { throw "$Name differs: $(Split-Path -Leaf $SourcePath) $a, $(Split-Path -Leaf $ToolPath) $b" }
    Write-Host "  $Name $b matches $(Split-Path -Leaf $SourcePath)"
}
$blobs = Join-Path $srcDir 'kmt_blobs.h'
$contract = Join-Path $repo 'driver\contract\bc250_umd_submit.h'
$caps = Join-Path $repo 'driver\contract\bc250_umd_private.h'
Assert-Same $contract '#define\s+BC250_UMD_ALLOC_MAGIC\s+(0x[0-9A-Fa-f]+u?)' $blobs '#define\s+BC250HSA_ALLOC_MAGIC\s+(0x[0-9A-Fa-f]+u?)' 'BC2A magic'
Assert-Same $contract '#define\s+BC250_UMD_CONTEXT_MAGIC\s+(0x[0-9A-Fa-f]+u?)' $blobs '#define\s+BC250HSA_CONTEXT_MAGIC\s+(0x[0-9A-Fa-f]+u?)' 'BC2C magic'
Assert-Same $contract '#define\s+BC250_UMD_SUBMIT_MAGIC\s+(0x[0-9A-Fa-f]+u?)' $blobs '#define\s+BC250HSA_SUBMIT_MAGIC\s+(0x[0-9A-Fa-f]+u?)' 'BC2S magic'
Assert-Same $caps '#define\s+BC250_UMD_PRIVATE_MAGIC\s+(0x[0-9A-Fa-f]+u?)' $blobs '#define\s+BC250HSA_CAPS_MAGIC\s+(0x[0-9A-Fa-f]+u?)' 'BC25 magic'
Assert-Same $contract '#define\s+BC250_UMD_ALLOC_SIZE_V1\s+(\d+)' $blobs '#define\s+BC250HSA_ALLOC_BLOB_BYTES\s+(\d+)u?' 'BC2A size'
Assert-Same $contract '#define\s+BC250_UMD_CONTEXT_SIZE_V2\s+(\d+)' $blobs '#define\s+BC250HSA_CONTEXT_BLOB_BYTES\s+(\d+)u?' 'BC2C size'
Assert-Same $contract '#define\s+BC250_UMD_SUBMIT_SIZE_V1\s+(\d+)' $blobs '#define\s+BC250HSA_SUBMIT_BLOB_BYTES\s+(\d+)u?' 'BC2S size'
Assert-Same $contract '#define\s+BC250_UMD_SUBMIT_MAX_IBS\s+(\d+)u?' $blobs '#define\s+BC250HSA_SUBMIT_MAX_IBS\s+(\d+)u?' 'BC2S max IBs'
Assert-Same $caps '#define\s+BC250_UMD_PRIVATE_SIZE_V3\s+(\d+)' $blobs '#define\s+BC250HSA_CAPS_BYTES\s+(\d+)u?' 'caps blob size'

# tools\run-lab.ps1 runs under Windows PowerShell 5.1 on the lab: it must at least parse.
$runner = Join-Path $toolDir 'run-lab.ps1'
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($runner, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { $errors | ForEach-Object { Write-Host "  $_" }; throw 'tools\run-lab.ps1 does not parse' }
Write-Host '  tools\run-lab.ps1 parses'

# ---------------------------------------------------------------------------------------------
# Gate 3: the fixtures are hash pinned, so the pin must hold. Every size and SHA-256 that
# tests\data\PROVENANCE.txt and PROVENANCE-runtime.txt state about a file of that directory is
# compared with the file. A fixture is a measurement, and an edit that leaves its record behind
# makes the record worthless: pm4_vadd.golden.txt once carried a stale pin that nothing read.
# ---------------------------------------------------------------------------------------------
$pinned = 0
$leaves = @{}
foreach ($file in Get-ChildItem -LiteralPath $dataDir -File) { $leaves[$file.Name] = $file.FullName }
foreach ($note in @('PROVENANCE.txt', 'PROVENANCE-runtime.txt')) {
    $notePath = Join-Path $dataDir $note
    if (-not (Test-Path $notePath)) { throw "$notePath is missing" }
    $current = $null
    foreach ($line in (Get-Content -LiteralPath $notePath)) {
        # The longest name a line holds, so that one file name inside another does not win.
        $named = $null
        foreach ($name in $leaves.Keys) {
            if ($line -match [regex]::Escape($name)) {
                if ($null -eq $named -or $name.Length -gt $named.Length) { $named = $name }
            }
        }
        if ($named) { $current = $named }
        if ($line -notmatch 'sha256\s+([0-9A-Fa-f]{64})') { continue }
        $stated = $Matches[1]
        if (-not $current) { throw "$note states a SHA-256 before it names a file: $line" }
        $actual = (Get-FileHash -Algorithm SHA256 $leaves[$current]).Hash
        if ($actual -ne $stated.ToUpperInvariant()) {
            throw "$note pins $current at sha256 $stated, the file is $actual"
        }
        if ($line -match '(\d+)\s+bytes') {
            $bytes = (Get-Item -LiteralPath $leaves[$current]).Length
            if ([int64]$Matches[1] -ne $bytes) {
                throw "$note pins $current at $($Matches[1]) bytes, the file is $bytes bytes"
            }
        }
        $pinned++
    }
}
if ($pinned -eq 0) { throw 'the PROVENANCE notes of tests\data pin no fixture at all' }
Write-Host "  $pinned fixture hashes of tests\data match PROVENANCE.txt and PROVENANCE-runtime.txt"

# ---------------------------------------------------------------------------------------------
# The test code objects. Committed under tests\data so the gate runs with no AMDGPU compiler.
# ---------------------------------------------------------------------------------------------
$kernelSource = Join-Path $dataDir 'm16_kernels.hip'
$codeObject = Join-Path $dataDir 'm16_kernels.gfx1013.co'
$fatBinary = Join-Path $dataDir 'm16_kernels.fatbin'
if ($Rebuild) {
    $clangExe = Join-Path $Clang 'clang.exe'
    if (-not (Test-Path $clangExe)) { throw "no portable AMDGPU clang at $Clang (-Rebuild needs it)" }
    # The two files are rebuilt at their own paths, not in a scratch folder: clang names
    # the module identifier symbol __hip_cuid_<hash> from the compilation, and the hash
    # follows the output path. A build of the same source into another directory
    # therefore differs in .dynstr and in the two hash tables, which is measured, not
    # guessed (PROVENANCE.txt states it).
    $before = @{}
    foreach ($f in @($codeObject, $fatBinary)) {
        if (Test-Path $f) { $before[$f] = (Get-FileHash -Algorithm SHA256 $f).Hash }
    }
    # The leaf names and this working directory, exactly as PROVENANCE.txt states them:
    # the identifier hash follows the output path as it is written on the command line,
    # so an absolute path would give another binary of the same size.
    Push-Location $dataDir
    try {
        & $clangExe -x hip --offload-arch=gfx1013 --offload-device-only --no-gpu-bundle-output `
            -nogpuinc -nogpulib -O2 -std=c++17 -o 'm16_kernels.gfx1013.co' 'm16_kernels.hip'
        if ($LASTEXITCODE -ne 0) { throw "clang failed for the code object ($LASTEXITCODE)" }
        & $clangExe -x hip --offload-arch=gfx1013 --offload-device-only `
            -nogpuinc -nogpulib -O2 -std=c++17 -o 'm16_kernels.fatbin' 'm16_kernels.hip'
        if ($LASTEXITCODE -ne 0) { throw "clang failed for the fat binary ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    foreach ($f in @($codeObject, $fatBinary)) {
        $fresh = (Get-FileHash -Algorithm SHA256 $f).Hash
        $name = Split-Path -Leaf $f
        if (-not $before.ContainsKey($f)) {
            Write-Host "  built $name (sha256 $fresh)"
        } elseif ($fresh -ne $before[$f]) {
            Write-Host "  rebuilt $name (sha256 changed to $fresh); update PROVENANCE.txt and the"
            Write-Host '    measured constants of the host tests'
        } else {
            Write-Host "  $name is byte for byte the committed file"
        }
    }
}
foreach ($f in @($codeObject, $fatBinary)) {
    if (-not (Test-Path $f)) { throw "$f is missing; run with -Rebuild and the portable clang present" }
}

# ---------------------------------------------------------------------------------------------
# The library, the tests and the tool
# ---------------------------------------------------------------------------------------------
$includes = @(
    "/I$(Join-Path $msvc.FullName 'include')",
    "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared",
    "/I$(Join-Path $here 'include')",
    "/I$srcDir"
)
# /Brepro on both the compiler and the linker: the artifacts carry no timestamp, so the
# SHA-256 printed at the end names the input and not the hour of the build.
$common = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/std:c11', '/Brepro',
    '/D_CRT_SECURE_NO_WARNINGS') + $includes
$link = @('/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')",
    "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")

function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @Arguments | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $What ($LASTEXITCODE)" }
}

# The host half: no display driver interface call, so every host test links it.
$hostSources = @('status.c', 'co_msgpack.c', 'co_metadata.c', 'co_loader.c', 'kernarg.c', 'pm4_dispatch.c')
# The Windows half.
$osSources = @('kmt_device.c', 'kmt_memory.c', 'submit.c')
$objDir = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null

$allObjects = @()
foreach ($s in ($hostSources + $osSources)) {
    Invoke-Cl ($common + @('/c', "/Fo$objDir\", (Join-Path $srcDir $s))) $s
    $allObjects += (Join-Path $objDir ([IO.Path]::ChangeExtension($s, 'obj')))
}
# /Brepro here as well, or the archive member headers carry the hour of the build and the
# library's SHA-256 changes with no change of its content. The archive member names are the
# object paths as they are written on the command line, so the call runs in the object
# directory and names the leaves, and no build path reaches the archive directory.
#
# This does not make bc250hsa.lib the same bytes at every -Out. Each object records its own
# absolute path in its .debug$S section, which the compiler writes even with no debug
# information asked for and with a relative /Fo (measured both ways, 2026-10-08). The seven
# programs and the two device artifacts are the same bytes at any -Out; the library's hash
# belongs to the directory it was built in, which the line below prints beside it.
$libObjects = @($allObjects | ForEach-Object { Split-Path -Leaf $_ })
Push-Location $objDir
try {
    & $lib /nologo /Brepro "/OUT:$Out\bc250hsa.lib" @libObjects | Out-Null
} finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) { throw "lib failed ($LASTEXITCODE)" }
Write-Host '  bc250hsa.lib built'

# The mock device half. Layer 2 links tests\host\bc250hsa_mock.c in place of the three
# Windows files, so layer 1 compiles it here: a signature that drifts away from
# include\bc250hsa.h fails this build and not layer 2's.
Invoke-Cl ($common + @("/I$testDir", '/c', "/Fo$objDir\",
    (Join-Path $testDir 'bc250hsa_mock.c'))) 'bc250hsa_mock.c'
Write-Host '  tests\host\bc250hsa_mock.c compiles against the frozen interface'

# The host tests link the host half only, plus the vendored Linux headers for the PM4 gate.
$hostObjects = @()
foreach ($s in $hostSources) { $hostObjects += (Join-Path $objDir ([IO.Path]::ChangeExtension($s, 'obj'))) }
# /I$here as well: test_vadd_oracle.c includes samples\vadd_expect.h, the oracle the HIP sample
# of layer 2 uses, so the criterion and the sample cannot drift apart.
$testIncludes = $includes + @("/I$(Join-Path $repo 'driver\amdgpu-import')", "/I$(Join-Path $repo 'third_party\linux-amdgpu')", "/I$here")
$tests = @('test_loader.c', 'test_unbundle.c', 'test_kernarg.c', 'test_descriptor.c', 'test_pm4.c',
    'test_vadd_oracle.c')
foreach ($t in $tests) {
    $exe = Join-Path $Out ([IO.Path]::ChangeExtension($t, 'exe'))
    Invoke-Cl (@('/nologo', '/W4', '/WX', '/O2', '/MT', '/std:c11', '/Brepro',
            '/D_CRT_SECURE_NO_WARNINGS') + $testIncludes +
        @("/Fo$objDir\", "/Fe$exe", (Join-Path $testDir $t)) + $hostObjects + @('/link') + $link) $t
}

# The step-1 tool.
Invoke-Cl ($common + @("/Fo$objDir\", "/Fe$Out\hipprobe.exe", (Join-Path $toolDir 'hipprobe.c')) +
    $allObjects + @('/link') + $link + @('gdi32.lib')) 'hipprobe'

if (-not $SkipTests) {
    $failures = 0
    foreach ($t in $tests) {
        $exe = Join-Path $Out ([IO.Path]::ChangeExtension($t, 'exe'))
        & $exe $dataDir
        if ($LASTEXITCODE -ne 0) { Write-Host "  FAIL $t ($LASTEXITCODE)"; $failures++ }
    }
    & "$Out\hipprobe.exe" --selftest $dataDir
    if ($LASTEXITCODE -ne 0) { Write-Host "  FAIL hipprobe --selftest ($LASTEXITCODE)"; $failures++ }
    & "$Out\hipprobe.exe" --help | Select-Object -First 1 | ForEach-Object { Write-Host "  $_" }

    # ------------------------------------------------------------------------------------------
    # The lab wrappers of lab\ and their failure paths (audit finding HIP-F2, 2026-10-10). No
    # GPU, no lab and no driver: each case replaces the sampler, the launcher or the terminator
    # with a fake, among them a helper that hangs, a sampler that answers nothing and a child
    # that never stops by itself. It runs under Windows PowerShell, which is the shell of the
    # lab, so the scripts are exercised by the shell that will run them.
    # ------------------------------------------------------------------------------------------
    $labTest = Join-Path $here 'tests\lab\test-arm-bounds.ps1'
    $labWork = Join-Path $Out 'lab-tests'
    $labLines = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $labTest -WorkDir $labWork 2>&1
    $labExit = $LASTEXITCODE
    $labLines | ForEach-Object { if ($_ -match 'FAIL|checks,') { Write-Host "  $_" } }
    if ($labExit -ne 0) { Write-Host "  FAIL test-arm-bounds ($labExit)"; $failures++ }

    if ($failures -gt 0) { throw "$failures host check(s) failed" }
    Write-Host '  every host test, hipprobe --selftest and the lab wrapper tests passed'
}

Write-Host "  artifacts in $Out (bc250hsa.lib's hash belongs to this directory, see above)"
foreach ($f in (@('bc250hsa.lib', 'hipprobe.exe') + ($tests | ForEach-Object { [IO.Path]::ChangeExtension($_, 'exe') }))) {
    $item = Get-Item (Join-Path $Out $f)
    $hash = (Get-FileHash -Algorithm SHA256 $item.FullName).Hash
    '{0,9}  {1}  sha256 {2}' -f $item.Length, $item.Name, $hash
}
foreach ($f in @($codeObject, $fatBinary)) {
    $item = Get-Item $f
    '{0,9}  {1}  sha256 {2}' -f $item.Length, $item.Name, (Get-FileHash -Algorithm SHA256 $f).Hash
}
