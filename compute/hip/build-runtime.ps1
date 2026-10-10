# Builds layer 2 of the M16 HIP runtime: amdhip64.dll, its import library amdhip64.lib, the
# mock build of the same DLL, the host tests, and the real clang-built HIP programs that link
# against them (vadd, hipthreads and the microbenchmark hipbench).
# Design: docs/design/m16-hip-route-b.md, sections 4, 5 and 9.
#
# Nothing is installed and nothing is written to drive C:. The compiler comes from the
# installed Visual Studio toolset, the headers and import libraries from the SDK NuGet packages
# under -Kits, exactly as tools\win\monfence\build.ps1 does. The portable AMDGPU clang is used
# for device-side artifacts only (the import library through llvm-dlltool, and the HIP sample);
# the script says so and goes on when it is absent.
#
#   pwsh bc250-win\compute\hip\build-runtime.ps1
#   pwsh bc250-win\compute\hip\build-runtime.ps1 -Bc250hsaLib P:\bc-250\scratch\build\m16-hip\bc250hsa.lib
#   pwsh bc250-win\compute\hip\build-runtime.ps1 -Rebuild
#
# -Bc250hsaLib names the static library of layer 1 (branch m16/hip-dispatch). Without it the
# script builds the mock DLL and every test, and it says that the product DLL needs that
# library.
#
# -Rebuild compiles the committed code object fixture again with the AMDGPU clang and compares
# its SHA-256 with the hash in tests\data\PROVENANCE-runtime.txt. It is off by default, because
# the fixture exists so that the host test runs on a machine with no AMDGPU compiler.

# $Root is the workspace that holds toolchain\nuget and scratch. The repository is checked out
# both as itself and as worktrees under scratch, so the depth from this script to the workspace
# is not fixed: the default walks up until it finds the toolchain, as build.ps1 of layer 1 does.
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
    [string]$Bc250hsaLib = '',
    [string]$ClangBin = '',
    [string]$DeviceLibPath = '',
    [switch]$SkipClang,
    [switch]$SkipTests,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\m16-hip-runtime' }
if (-not $ClangBin) { $ClangBin = Join-Path $Root 'toolchain\llvm-amdgpu-22.1.8\mingw64\bin' }
$hip = Join-Path $repo 'compute\hip'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found at $cl" }
New-Item -ItemType Directory -Force $Out | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Out 'mock') | Out-Null
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$def = Join-Path $hip 'runtime\amdhip64.def'
$header = Join-Path $hip 'include\hip\hip_runtime.h'
$fixture = Join-Path $hip 'tests\data\hip_test_kernels.gfx1013.fatbin'

# ---------------------------------------------------------------------------------------------
# 1. The export list: the module definition file, the header and the built DLL must agree.
# ---------------------------------------------------------------------------------------------
# 53 HIP entry points (sections 4.1 and 4.9) and the two counter calls of runtime\hip_perf.cpp
# (section 8.7), which are ours and not HIP.
$expectedExports = 55
$defNames = Get-Content $def | Where-Object { $_ -match '^[A-Za-z_]' -and $_ -notmatch '^(LIBRARY|EXPORTS)' } | ForEach-Object { $_.Trim() }
Write-Host "  amdhip64.def holds $($defNames.Count) names"
if ($defNames.Count -ne $expectedExports) {
    throw "amdhip64.def holds $($defNames.Count) names, and sections 4.1, 4.9 and 8.7 of the design state $expectedExports"
}
$headerText = Get-Content $header -Raw
foreach ($name in $defNames) {
    if ($headerText -notmatch "(?m)\b$([regex]::Escape($name))\s*\(") {
        throw "the exported name $name has no declaration in hip_runtime.h"
    }
}
Write-Host '  every exported name is declared in hip_runtime.h'

# ---------------------------------------------------------------------------------------------
# 2. Compile the runtime, the mock backend and the test.
# ---------------------------------------------------------------------------------------------
$includes = @("/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared",
    "/I$(Join-Path $hip 'include')", "/I$(Join-Path $hip 'runtime')",
    "/I$(Join-Path $hip 'tests\host')")
$libpaths = @("/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64",
    "/LIBPATH:$sdkLib\um\x64")
# /Brepro on the compiler and on the linker, as build.ps1 of layer 1 does: the artifacts carry
# no build timestamp, so the hash of a DLL names its source and not the hour. MEASURED: without
# it, two builds of the same commit gave two hashes of amdhip64.dll, which makes the hash in a
# lab kit worthless.
$libpaths = @('/Brepro') + $libpaths
$warn = @('/nologo', '/W4', '/WX', '/O2', '/Brepro', '/D_CRT_SECURE_NO_WARNINGS', '/DBC250_HIP_BUILD_DLL')

function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    $spoken = @()
    & $cl @Arguments | ForEach-Object {
        $spoken += $_
        if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.(c|cpp)$|^\s+Creating library') { Write-Host "  $_" }
    }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $What ($LASTEXITCODE)" }
    # /WX makes the compiler strict; the linker needs its own rule. LNK4098 is a C runtime
    # mismatch: one input asks for the static runtime and another for the dynamic one, so the
    # module ends with two C runtimes, two heaps and two copies of errno. The code is
    # locale independent, the message beside it is not.
    foreach ($line in $spoken) {
        if ($line -match 'LNK4098') {
            throw "the link of $What mixes two C runtimes (LNK4098): $line"
        }
    }
}

$runtimeSources = @('hip_device.cpp', 'hip_error.cpp', 'hip_event.cpp', 'hip_launch.cpp',
    'hip_log.cpp', 'hip_memory.cpp', 'hip_module.cpp', 'hip_perf.cpp', 'hip_stream.cpp') |
    ForEach-Object { Join-Path $hip "runtime\$_" }
$dllSource = Join-Path $hip 'runtime\dllmain.cpp'
$mockSource = Join-Path $hip 'tests\host\hipmock_backend.c'
$testSource = Join-Path $hip 'tests\host\test_hip_mock.cpp'

# The static objects of the runtime, for the host test (/MT, no DLL export).
$objDir = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null
Invoke-Cl ($warn + @('/c', '/MT', '/std:c++17', '/EHsc', "/Fo$objDir\") + $includes + $runtimeSources) 'runtime objects (static)'
Invoke-Cl ($warn + @('/c', '/MT', '/std:c11', "/Fo$objDir\") + $includes + @($mockSource)) 'mock backend (static)'

# The seam of design section 1: layer 2 uses bc250hsa.h and nothing else of the submission
# path. Every undefined symbol of the runtime objects whose name belongs to our own stack must
# therefore be declared in bc250hsa.h. A new call into layer 1 that the header does not carry
# is a build failure here.
if (Test-Path $dumpbin) {
    $contractText = Get-Content (Join-Path $hip 'include\bc250hsa.h') -Raw
    $ours = @()
    foreach ($line in (& $dumpbin /nologo /symbols (Join-Path $objDir '*.obj'))) {
        if ($line -match 'UNDEF\b.*\|\s+(\S+)$') {
            $name = $Matches[1]
            if ($name -match '^_?(bc250|hsa_|amd)') { $ours += $name }
        }
    }
    $ours = $ours | Sort-Object -Unique
    foreach ($name in $ours) {
        if ($contractText -notmatch "(?m)\b$([regex]::Escape($name))\s*\(") {
            throw "the runtime calls $name, which bc250hsa.h does not declare"
        }
    }
    Write-Host "  the runtime calls $($ours.Count) names of our own stack, all of them declared in bc250hsa.h"
}

if (-not $SkipTests) {
    $testExe = Join-Path $Out 'test_hip_mock.exe'
    $testObjDir = Join-Path $Out 'obj-test'
    New-Item -ItemType Directory -Force $testObjDir | Out-Null
    $objects = Get-ChildItem $objDir -Filter '*.obj' | ForEach-Object { $_.FullName }
    Invoke-Cl ($warn + @('/MT', '/std:c++17', '/EHsc', "/Fo$testObjDir\", "/Fe$testExe", $testSource) +
        $includes + $objects + @('/link') + $libpaths) 'test_hip_mock'
    & $testExe $fixture
    if ($LASTEXITCODE -ne 0) { throw "test_hip_mock failed ($LASTEXITCODE)" }

    # ------------------------------------------------------------------------------------------
    # The multithreaded client, and its negative control.
    #
    # The owner asks for multithreading to be measured with our own clients (2026-09-29). The
    # control build compiles the same runtime with BC250_HIP_WAIT_UNDER_LOCK=1, which restores
    # the process lock over the waits, and the test refuses to run as a control unless the
    # runtime it links really behaves that way. Without the control, a green run of the test
    # would say nothing: a test that measures concurrency has to fail when there is none.
    # ------------------------------------------------------------------------------------------
    $threadsTestSource = Join-Path $hip 'tests\host\test_hip_threads.cpp'
    $threadsExe = Join-Path $Out 'test_hip_threads.exe'
    Invoke-Cl ($warn + @('/MT', '/std:c++17', '/EHsc', "/Fo$testObjDir\", "/Fe$threadsExe",
        $threadsTestSource) + $includes + $objects + @('/link') + $libpaths) 'test_hip_threads'
    & $threadsExe $fixture
    if ($LASTEXITCODE -ne 0) { throw "test_hip_threads failed ($LASTEXITCODE)" }

    $ctlObjDir = Join-Path $Out 'obj-ctl'
    New-Item -ItemType Directory -Force $ctlObjDir | Out-Null
    Invoke-Cl ($warn + @('/c', '/MT', '/std:c++17', '/EHsc', '/DBC250_HIP_WAIT_UNDER_LOCK=1',
        "/Fo$ctlObjDir\") + $includes + $runtimeSources) 'runtime objects (wait under the lock)'
    Invoke-Cl ($warn + @('/c', '/MT', '/std:c11', "/Fo$ctlObjDir\") + $includes + @($mockSource)) 'mock backend (control)'
    $ctlObjects = Get-ChildItem $ctlObjDir -Filter '*.obj' | ForEach-Object { $_.FullName }
    $ctlExe = Join-Path $Out 'test_hip_threads_control.exe'
    Invoke-Cl ($warn + @('/MT', '/std:c++17', '/EHsc', '/DBC250_HIP_WAIT_UNDER_LOCK=1',
        "/Fo$ctlObjDir\", "/Fe$ctlExe", $threadsTestSource) + $includes + $ctlObjects +
        @('/link') + $libpaths) 'test_hip_threads_control'
    & $ctlExe $fixture '--negative-control'
    if ($LASTEXITCODE -ne 0) { throw "the negative control of test_hip_threads failed ($LASTEXITCODE)" }

    # ------------------------------------------------------------------------------------------
    # Batching, from the side layer 2 sees it (design section 9). The dwords of a batched indirect
    # buffer are the business of layer 1's test_pm4; this one drives the flush points, where a
    # mistake is a wait that never ends rather than a slow program.
    # ------------------------------------------------------------------------------------------
    $batchTestSource = Join-Path $hip 'tests\host\test_hip_batch.cpp'
    $batchExe = Join-Path $Out 'test_hip_batch.exe'
    Invoke-Cl ($warn + @('/MT', '/std:c++17', '/EHsc', "/Fo$testObjDir\", "/Fe$batchExe",
        $batchTestSource) + $includes + $objects + @('/link') + $libpaths) 'test_hip_batch'
    & $batchExe $fixture
    if ($LASTEXITCODE -ne 0) { throw "test_hip_batch failed ($LASTEXITCODE)" }

    # ------------------------------------------------------------------------------------------
    # And every other host test again with batching on, through the environment the runtime
    # reads (runtime\hip_device.cpp). The default of this build is off, so without this pass
    # nothing but the test above would ever run a batched submission.
    # ------------------------------------------------------------------------------------------
    $env:BC250_HIP_BATCH = '1'
    $env:BC250_HIP_BARRIER = 'light'
    try {
        foreach ($pair in @(@{ exe = $testExe; name = 'test_hip_mock' },
                            @{ exe = $threadsExe; name = 'test_hip_threads' })) {
            & $pair.exe $fixture
            if ($LASTEXITCODE -ne 0) {
                throw "$($pair.name) failed with BC250_HIP_BATCH=1 ($LASTEXITCODE)"
            }
            Write-Host "  $($pair.name) passes with batching on and the light barrier"
        }
    } finally {
        Remove-Item env:BC250_HIP_BATCH -ErrorAction SilentlyContinue
        Remove-Item env:BC250_HIP_BARRIER -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------------------------
# 3. The mock build of the DLL: the same runtime over the mock backend, so that a real HIP
#    program runs on a machine with no BC-250 adapter.
# ---------------------------------------------------------------------------------------------
$mockObjDir = Join-Path $Out 'obj-mock'
New-Item -ItemType Directory -Force $mockObjDir | Out-Null
$mockDll = Join-Path $Out 'mock\amdhip64.dll'
Invoke-Cl ($warn + @('/c', '/MD', '/std:c11', "/Fo$mockObjDir\") + $includes + @($mockSource)) 'mock backend (DLL)'
Invoke-Cl ($warn + @('/LD', '/MD', '/std:c++17', '/EHsc', "/Fo$mockObjDir\", "/Fe$mockDll") +
    $includes + $runtimeSources + @($dllSource, (Join-Path $mockObjDir 'hipmock_backend.obj')) +
    @('/link', "/DEF:$def") + $libpaths) 'amdhip64.dll (mock backend)'

# 3a. The negative control of the AQL dispatch packet (section 7.1 of bc250hsa.h, defect
#     BD-110). The same DLL over the same mock, built with BC250_HIP_NO_DISPATCH_PACKET=1, so
#     that it passes no packet address and every kernel which reads its own blockDim is refused
#     exactly as the build of 2026-10-09 refused it. A real HIP client of that shape can then be
#     run against both DLLs offline, and the difference is the fix and nothing else.
$noPktObjDir = Join-Path $Out 'obj-mock-nopacket'
New-Item -ItemType Directory -Force $noPktObjDir | Out-Null
$noPktDll = Join-Path $Out 'mock-no-dispatch-packet\amdhip64.dll'
New-Item -ItemType Directory -Force (Split-Path -Parent $noPktDll) | Out-Null
Invoke-Cl ($warn + @('/c', '/MD', '/std:c11', "/Fo$noPktObjDir\") + $includes + @($mockSource)) 'mock backend (no-packet control)'
Invoke-Cl ($warn + @('/LD', '/MD', '/std:c++17', '/EHsc', '/DBC250_HIP_NO_DISPATCH_PACKET=1',
    "/Fo$noPktObjDir\", "/Fe$noPktDll") +
    $includes + $runtimeSources + @($dllSource, (Join-Path $noPktObjDir 'hipmock_backend.obj')) +
    @('/link', "/DEF:$def") + $libpaths) 'amdhip64.dll (mock backend, no dispatch packet)'

# ---------------------------------------------------------------------------------------------
# 4. The product DLL, when the static library of layer 1 is available.
# ---------------------------------------------------------------------------------------------
$productDll = Join-Path $Out 'amdhip64.dll'
if ($Bc250hsaLib -and (Test-Path $Bc250hsaLib)) {
    $productObjDir = Join-Path $Out 'obj-product'
    New-Item -ItemType Directory -Force $productObjDir | Out-Null
    # gdi32.lib holds the D3DKMT* entry points that bc250hsa.lib calls. The mock build does not
    # need it, which is why the product link is the first one to ask for it.
    #
    # /MT and not /MD, for two reasons. First, build.ps1 of layer 1 builds bc250hsa.lib with
    # /MT, so a /MD link of this DLL pulls LIBCMT and MSVCRT into one module: the linker says
    # LNK4098 and the module gets two C runtimes, each with its own heap. Second, the DLL goes
    # to the lab beside a program: a /MT module needs no Visual C runtime on the target.
    Invoke-Cl ($warn + @('/LD', '/MT', '/std:c++17', '/EHsc', "/Fo$productObjDir\", "/Fe$productDll") +
        $includes + $runtimeSources + @($dllSource, $Bc250hsaLib) +
        @('/link', "/DEF:$def") + $libpaths + @('gdi32.lib')) 'amdhip64.dll (bc250hsa)'
} else {
    Write-Host '  amdhip64.dll over bc250hsa: skipped, -Bc250hsaLib names no library of layer 1'
}

# ---------------------------------------------------------------------------------------------
# 5. The exports of what was built must equal the module definition file.
# ---------------------------------------------------------------------------------------------
function Assert-Exports([string]$Path) {
    if (-not (Test-Path $dumpbin)) {
        Write-Host "  export check skipped: dumpbin.exe not found"
        return
    }
    $lines = & $dumpbin /nologo /exports $Path
    $found = @()
    foreach ($line in $lines) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $found += $Matches[1] }
    }
    $missing = $defNames | Where-Object { $found -notcontains $_ }
    $extra = $found | Where-Object { $defNames -notcontains $_ }
    if ($missing) { throw "$(Split-Path -Leaf $Path) does not export: $($missing -join ', ')" }
    if ($extra) { throw "$(Split-Path -Leaf $Path) exports more than the definition file: $($extra -join ', ')" }
    Write-Host "  $(Split-Path -Leaf $Path) exports exactly the $($found.Count) names of amdhip64.def"
}
Assert-Exports $mockDll
if (Test-Path $productDll) { Assert-Exports $productDll }

# ---------------------------------------------------------------------------------------------
# 6. The import library, and a real HIP program against it.
# ---------------------------------------------------------------------------------------------
$clang = Join-Path $ClangBin 'clang.exe'
$dlltool = Join-Path $ClangBin 'llvm-dlltool.exe'
if ($SkipClang -or -not (Test-Path $clang)) {
    Write-Host "  the AMDGPU clang is absent ($clang): the import library and the HIP sample are skipped"
} else {
    # MEASURED: clang's HIP driver appends a bare amdhip64.lib to the Windows link line, so the
    # import library must carry exactly that name and sit on the library search path. The
    # library must hold the jump thunks as well as the __imp_ symbols, which llvm-dlltool
    # writes and a hand-made library of __imp_ symbols does not.
    $implib = Join-Path $Out 'amdhip64.lib'
    & $dlltool -m i386:x86-64 -d $def -l $implib -D amdhip64.dll
    if ($LASTEXITCODE -ne 0) { throw "llvm-dlltool failed ($LASTEXITCODE)" }
    Copy-Item $implib (Join-Path $Out 'mock\amdhip64.lib') -Force
    Write-Host "  amdhip64.lib $((Get-Item $implib).Length) bytes"

    # MEASURED condition: clang compiles the device pass with the host include search path as
    # well, so the MSVC and Windows SDK include and library paths must be in the environment.
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
    $savedPath = $env:PATH
    & cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^(INCLUDE|LIB|LIBPATH|UCRTVersion|WindowsSdkDir|WindowsSDKVersion)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
        if ($_ -match '^Path=(.*)$') { $env:PATH = "$ClangBin;$($Matches[1])" }
    }

    # The fixture of the host test, built again from its own source and checked by the host
    # test itself. MEASURED, which is why this is not a hash comparison: clang writes a unique
    # __hip_cuid_<16 hex> symbol into every compilation, so two runs of the same command on the
    # same compiler differ in 38 bytes (the symbol, the hash table it feeds and the symbol
    # order). The fixture is reproducible in what the test reads of it, not byte for byte.
    if ($Rebuild) {
        $again = Join-Path $Out 'hip_test_kernels.gfx1013.fatbin'
        & $clang '-x' 'hip' '--offload-arch=gfx1013' '--offload-device-only' '-nogpuinc' `
            '-nogpulib' '-O2' '-std=c++17' "-I$(Join-Path $hip 'include')" '-o' $again `
            (Join-Path $hip 'tests\data\hip_test_kernels.hip')
        if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "clang failed for the fixture ($LASTEXITCODE)" }
        $builtSize = (Get-Item $again).Length
        $committedSize = (Get-Item $fixture).Length
        Write-Host "  the fixture rebuilt: $builtSize bytes, sha256 $((Get-FileHash -Algorithm SHA256 $again).Hash)"
        Write-Host "  the committed one:   $committedSize bytes, sha256 $((Get-FileHash -Algorithm SHA256 $fixture).Hash)"
        $testExe = Join-Path $Out 'test_hip_mock.exe'
        if (Test-Path $testExe) {
            & $testExe $again
            if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "the host test fails on the rebuilt fixture ($LASTEXITCODE)" }
            Write-Host '  the host test passes on the rebuilt fixture as well'
        } else {
            Write-Host '  the host test was not built (-SkipTests), so the rebuilt fixture is unchecked'
        }
    }

    $sampleExe = Join-Path $Out 'mock\vadd.exe'
    # MEASURED: -fms-runtime-lib=dll makes the host object import printf and malloc from the
    # C runtime DLL while clang still links the static libucrt, which the linker answers with
    # LNK4098 and LNK4217. The static C runtime is the default and it links clean. Our DLL has
    # its own C runtime, and nothing of the C runtime crosses the interface.
    # The sample is not byte reproducible, and -Xlinker /Brepro does not make it so. MEASURED:
    # two builds of the same source differ in 84 bytes, which are the unique __hip_cuid_<16 hex>
    # symbol that clang writes into every HIP compilation (twice in the image) and four bytes of
    # the PE header. The hash printed for vadd.exe therefore names one build of it. The hashes of
    # amdhip64.dll and of the tests do not have this property: they are reproducible.
    $flags = @('-x', 'hip', '--offload-arch=gfx1013', '--target=x86_64-pc-windows-msvc',
        '-nogpuinc', '-O2', '-std=c++17', "-I$(Join-Path $hip 'include')")
    if ($DeviceLibPath -and (Test-Path $DeviceLibPath)) {
        $flags += "--hip-device-lib-path=$DeviceLibPath"
    } else {
        # The sample calls no function of the ROCm device library, so it builds without one.
        $flags += '-nogpulib'
    }
    & $clang @flags -o $sampleExe (Join-Path $hip 'samples\vadd.hip') "-L$Out"
    if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "clang failed for the HIP sample ($LASTEXITCODE)" }

    # The import table must name our DLL and nothing of AMD.
    $objdump = Join-Path $ClangBin 'llvm-objdump.exe'
    $imports = & $objdump --private-headers $sampleExe | Select-String 'DLL Name'
    $imports | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    if (-not ($imports -match 'amdhip64.dll')) { $env:PATH = $savedPath; throw 'the sample does not import amdhip64.dll' }

    # The lab takes the sample from the output root, next to the product DLL. The mock build of
    # the DLL stays in mock\ and must never reach the lab: it runs no instruction, so a green
    # run of it would prove nothing about the hardware.
    Copy-Item $sampleExe (Join-Path $Out 'vadd.exe') -Force

    # Run it against the mock DLL, which sits next to the executable in mock\. --wait-total is
    # the switch that a lab trial uses to keep every wait short, so the build exercises it.
    $record = Join-Path $Out 'mock\record.txt'
    $env:BC250_HIP_MOCK_RECORD = $record
    $sampleOut = & $sampleExe '--wait-total' '10000' 2>&1
    $sampleExit = $LASTEXITCODE
    # The build path stays: the threads sample below is compiled by the same clang, and its link
    # needs the toolset the vcvars environment names.
    $sampleOut | ForEach-Object { Write-Host "  $_" }
    if ($sampleExit -ne 0) { throw "the HIP sample failed ($sampleExit)" }
    if (-not (Test-Path $record)) { throw 'the mock backend wrote no record of the sample run' }
    $lines = Get-Content $record
    $dispatches = $lines | Where-Object { $_ -match '^dispatch ' }
    # The kernels of the sample have C++ linkage, so the device name is the mangled name, such
    # as _Z4vaddPKfS0_Pfi. That is what a real HIP program looks like.
    $vaddRuns = ($dispatches | Where-Object { $_ -match 'kernel=\S*vadd\S*\s' }).Count
    $scaleRuns = ($dispatches | Where-Object { $_ -match 'kernel=\S*scale\S*\s' }).Count
    Write-Host "  the sample recorded $($dispatches.Count) dispatches: vadd $vaddRuns, scale $scaleRuns"
    if ($vaddRuns -ne 2 -or $scaleRuns -ne 2) {
        throw "the sample should record two vadd and two scale dispatches, and it recorded $vaddRuns and $scaleRuns"
    }
    # N / BLOCK = 65536 / 256 = 256 workgroups of 256 work items, and the kernel argument
    # buffer of vadd is 288 bytes: four explicit arguments and the hidden block.
    if (-not ($dispatches[0] -match 'grid=256,1,1 block=256,1,1')) {
        throw "the first dispatch does not carry the grid of the sample: $($dispatches[0])"
    }
    if (-not ($dispatches[0] -match 'kernarg=288 ')) {
        throw "the first dispatch does not carry 288 kernel argument bytes: $($dispatches[0])"
    }
    $waits = ($lines | Where-Object { $_ -match '^wait ' }).Count
    $bounded = ($lines | Where-Object { $_ -match '^wait .* wait=0/10000 ' }).Count
    Write-Host "  the sample recorded $waits waits, $bounded of them with the bound it was given"
    if ($waits -eq 0 -or $bounded -ne $waits) {
        throw "--wait-total did not reach every wait: $bounded of $waits carry the 10000 ms bound"
    }

    # ------------------------------------------------------------------------------------------
    # The multithreaded client as a real HIP program. It shares its measurement with the host
    # test (tests\host\hip_threads_client.h) and it is the program of the step-3 lab session:
    # there the delay of a wait is the kernel itself (--spin), and here it is the hold time of
    # the mock backend (--mock-hold).
    # ------------------------------------------------------------------------------------------
    $threadsSampleExe = Join-Path $Out 'mock\hipthreads.exe'
    $threadsFlags = $flags + @("-I$(Join-Path $hip 'tests\host')")
    & $clang @threadsFlags -o $threadsSampleExe (Join-Path $hip 'samples\threads.hip') "-L$Out"
    if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "clang failed for the threads sample ($LASTEXITCODE)" }
    Copy-Item $threadsSampleExe (Join-Path $Out 'hipthreads.exe') -Force

    $env:BC250_HIP_MOCK_RECORD = Join-Path $Out 'mock\record-threads.txt'
    # --device-overlaps, because this run is against the mock backend, which holds each dispatch
    # on a timer of its own and therefore has as many in flight as the threads give it. On the
    # lab the same program runs without the flag: one indirect buffer at a time (defect BD-111).
    $threadsOut = & $threadsSampleExe '--wait-total' '20000' '--mock-hold' '150' '--device-overlaps' 2>&1
    $threadsExit = $LASTEXITCODE
    $threadsOut | ForEach-Object { Write-Host "  $_" }
    if ($threadsExit -ne 0) { $env:PATH = $savedPath; throw "the threads sample failed ($threadsExit)" }
    $inWindow = $threadsOut | Select-String -Pattern 'operations in all' | ForEach-Object {
        if ($_.Line -match '(\d+) operations in all') { [int]$Matches[1] } }
    if (-not $inWindow -or $inWindow -lt 3) {
        throw "the threads sample measured $inWindow operations inside the long wait of another thread"
    }
    Write-Host "  the threads sample measured $inWindow operations inside one thread's wait for the device"

    # ------------------------------------------------------------------------------------------
    # The device-header contracts of hip/hip_fp16.h and hip/hip_runtime.h: the directed
    # float-to-half conversions and the block collectives (audit findings HS-1 and HS-2 of
    # 2026-10-10). The host half of this program needs no device, so the run below is a real
    # gate on the conversion arithmetic; the block collectives need the GPU and are checked on
    # the lab with --expect-compute.
    # ------------------------------------------------------------------------------------------
    $halfExe = Join-Path $Out 'mock\halfblock.exe'
    & $clang @flags -o $halfExe (Join-Path $hip 'samples\halfblock.hip') "-L$Out"
    if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "clang failed for halfblock ($LASTEXITCODE)" }
    Copy-Item $halfExe (Join-Path $Out 'halfblock.exe') -Force

    $env:BC250_HIP_MOCK_RECORD = Join-Path $Out 'mock\record-halfblock.txt'
    $halfOut = & $halfExe '--wait-total' '20000' 2>&1
    $halfExit = $LASTEXITCODE
    $halfOut | ForEach-Object { Write-Host "  $_" }
    if ($halfExit -ne 0) { $env:PATH = $savedPath; throw "halfblock failed against the mock DLL ($halfExit)" }
    $hostChecks = $halfOut | Select-String -Pattern 'host checks (\d+), failed (\d+)' |
        Select-Object -First 1
    if (-not $hostChecks) { $env:PATH = $savedPath; throw 'halfblock printed no host check count' }
    $ran = [int]$hostChecks.Matches[0].Groups[1].Value
    $failed = [int]$hostChecks.Matches[0].Groups[2].Value
    if ($ran -lt 100 -or $failed -ne 0) {
        $env:PATH = $savedPath
        throw "halfblock ran $ran host checks with $failed failed; the table is 108 checks and must pass"
    }
    Write-Host "  halfblock passed $ran host conversion checks with no device"

    # ------------------------------------------------------------------------------------------
    # The microbenchmark. On the lab it answers what one kernel launch costs off the GPU; here
    # it proves the harness and the counters, against the mock DLL, which runs no instruction.
    # The run is short on purpose: the numbers of a mock say nothing about the hardware, only
    # that every measurement and every counter delta comes out.
    # ------------------------------------------------------------------------------------------
    $benchExe = Join-Path $Out 'mock\hipbench.exe'
    & $clang @flags -o $benchExe (Join-Path $hip 'samples\hipbench.hip') "-L$Out"
    if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "clang failed for hipbench ($LASTEXITCODE)" }
    Copy-Item $benchExe (Join-Path $Out 'hipbench.exe') -Force

    $env:BC250_HIP_MOCK_RECORD = Join-Path $Out 'mock\record-bench.txt'
    $benchOut = & $benchExe '--wait-total' '20000' '--launches' '200' '--chain' '50' `
        '--sync' '50' '--event' '20' '--copy-iterations' '2' '--budget-ms' '60000' 2>&1
    $benchExit = $LASTEXITCODE
    $env:PATH = $savedPath
    $benchOut | ForEach-Object { Write-Host "  $_" }
    if ($benchExit -ne 0) { throw "hipbench failed against the mock DLL ($benchExit)" }
    # The one number this run is allowed to assert: the default of the runtime batches, so one
    # launch must be well under one submission. The default changed on 2026-10-09 with the lab
    # measurement behind it (evidence/m16/perf-2026-10-09), and this is the gate that says the
    # default really reaches a HIP program through the counters of the DLL.
    $perLaunch = $benchOut | Select-String -Pattern 'submissions per launch' | ForEach-Object {
        if ($_.Line -match '([0-9.]+) submissions per launch') { [double]$Matches[1] } }
    if (-not $perLaunch) { throw 'hipbench printed no submissions per launch: the counters are not reaching it' }
    Write-Host "  hipbench read $($perLaunch.Count) counter ratios from the DLL, the first $($perLaunch[0])"
    if ($perLaunch[0] -gt 0.1) {
        throw "the default of the runtime batches, so one launch must be under 0.1 submissions, and hipbench measured $($perLaunch[0])"
    }

    # The same arm with the two switches turned off, which is exactly build 1: one launch, one
    # submission. The switches are what a lab arm compares against, so a build in which they
    # stopped working would be a build whose defaults cannot be measured.
    $env:BC250_HIP_MOCK_RECORD = Join-Path $Out 'mock\record-bench-nobatch.txt'
    $benchPlainOut = & $benchExe '--wait-total' '20000' '--launches' '200' '--chain' '50' `
        '--sync' '50' '--event' '20' '--copy-iterations' '1' '--batch' '0' `
        '--barrier' 'full' '--budget-ms' '60000' 2>&1
    $benchPlainExit = $LASTEXITCODE
    $benchPlainOut | ForEach-Object { Write-Host "  $_" }
    if ($benchPlainExit -ne 0) { throw "hipbench with batching off failed ($benchPlainExit)" }
    $plainPerLaunch = $benchPlainOut | Select-String -Pattern 'submissions per launch' |
        ForEach-Object { if ($_.Line -match '([0-9.]+) submissions per launch') { [double]$Matches[1] } }
    if (-not $plainPerLaunch) { throw 'hipbench with batching off printed no submissions per launch' }
    Write-Host "  hipbench with batching off measured $($plainPerLaunch[0]) submissions per launch"
    if ([math]::Abs($plainPerLaunch[0] - 1.0) -gt 0.01) {
        throw "with batching off one launch must be one submission, and hipbench measured $($plainPerLaunch[0])"
    }
}

# ---------------------------------------------------------------------------------------------
# 7. What was built.
# ---------------------------------------------------------------------------------------------
Write-Host ''
foreach ($file in @((Join-Path $Out 'mock\amdhip64.dll'), (Join-Path $Out 'amdhip64.dll'),
        (Join-Path $Out 'amdhip64.lib'), (Join-Path $Out 'test_hip_mock.exe'),
        (Join-Path $Out 'test_hip_threads.exe'), (Join-Path $Out 'test_hip_threads_control.exe'),
        (Join-Path $Out 'test_hip_batch.exe'),
        (Join-Path $Out 'vadd.exe'), (Join-Path $Out 'mock\vadd.exe'),
        (Join-Path $Out 'hipthreads.exe'), (Join-Path $Out 'mock\hipthreads.exe'),
        (Join-Path $Out 'hipbench.exe'), (Join-Path $Out 'mock\hipbench.exe'),
        (Join-Path $Out 'halfblock.exe'), (Join-Path $Out 'mock\halfblock.exe'))) {
    if (-not (Test-Path $file)) { continue }
    $item = Get-Item $file
    $label = $item.FullName.Substring($Out.Length).TrimStart('\')
    '{0,9}  {1,-26}  sha256 {2}' -f $item.Length, $label, (Get-FileHash -Algorithm SHA256 $item.FullName).Hash
}
