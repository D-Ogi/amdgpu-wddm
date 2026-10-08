# Builds layer 2 of the M16 HIP runtime: amdhip64.dll, its import library amdhip64.lib, the
# mock build of the same DLL, the host test, and a real clang-built HIP program that links
# against them. Design: docs/design/m16-hip-route-b.md, sections 4 and 5.
#
# Nothing is installed and nothing is written to drive C:. The compiler comes from the
# installed Visual Studio toolset, the headers and import libraries from the SDK NuGet packages
# under -Kits, exactly as tools\win\monfence\build.ps1 does. The portable AMDGPU clang is used
# for device-side artifacts only (the import library through llvm-dlltool, and the HIP sample);
# the script says so and goes on when it is absent.
#
#   pwsh bc250-win\compute\hip\build-runtime.ps1
#   pwsh bc250-win\compute\hip\build-runtime.ps1 -Bc250hsaLib P:\bc-250\scratch\build\m16-hip\bc250hsa.lib
#
# -Bc250hsaLib names the static library of layer 1 (branch m16/hip-dispatch). Without it the
# script builds the mock DLL and every test, and it says that the product DLL needs that
# library.

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path }),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0',
    [string]$Bc250hsaLib = '',
    [string]$ClangBin = '',
    [string]$DeviceLibPath = '',
    [switch]$SkipClang,
    [switch]$SkipTests
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
$expectedExports = 38
$defNames = Get-Content $def | Where-Object { $_ -match '^[A-Za-z_]' -and $_ -notmatch '^(LIBRARY|EXPORTS)' } | ForEach-Object { $_.Trim() }
Write-Host "  amdhip64.def holds $($defNames.Count) names"
if ($defNames.Count -ne $expectedExports) {
    throw "amdhip64.def holds $($defNames.Count) names, and section 4.1 of the design states $expectedExports"
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
$warn = @('/nologo', '/W4', '/WX', '/O2', '/D_CRT_SECURE_NO_WARNINGS', '/DBC250_HIP_BUILD_DLL')

function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @Arguments | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.(c|cpp)$|^\s+Creating library') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $What ($LASTEXITCODE)" }
}

$runtimeSources = @('hip_device.cpp', 'hip_error.cpp', 'hip_event.cpp', 'hip_launch.cpp',
    'hip_memory.cpp', 'hip_module.cpp', 'hip_stream.cpp') |
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

# ---------------------------------------------------------------------------------------------
# 4. The product DLL, when the static library of layer 1 is available.
# ---------------------------------------------------------------------------------------------
$productDll = Join-Path $Out 'amdhip64.dll'
if ($Bc250hsaLib -and (Test-Path $Bc250hsaLib)) {
    $productObjDir = Join-Path $Out 'obj-product'
    New-Item -ItemType Directory -Force $productObjDir | Out-Null
    Invoke-Cl ($warn + @('/LD', '/MD', '/std:c++17', '/EHsc', "/Fo$productObjDir\", "/Fe$productDll") +
        $includes + $runtimeSources + @($dllSource, $Bc250hsaLib) +
        @('/link', "/DEF:$def") + $libpaths) 'amdhip64.dll (bc250hsa)'
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

    $sampleExe = Join-Path $Out 'mock\vadd.exe'
    # MEASURED: -fms-runtime-lib=dll makes the host object import printf and malloc from the
    # C runtime DLL while clang still links the static libucrt, which the linker answers with
    # LNK4098 and LNK4217. The static C runtime is the default and it links clean. Our DLL has
    # its own C runtime, and nothing of the C runtime crosses the interface.
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

    # Run it against the mock DLL, which sits next to the executable.
    $record = Join-Path $Out 'mock\record.txt'
    $env:BC250_HIP_MOCK_RECORD = $record
    # The mock runs no instruction, so the sample must not ask for the sums.
    $env:BC250_HIP_EXPECT_COMPUTE = ''
    $sampleOut = & $sampleExe 2>&1
    $sampleExit = $LASTEXITCODE
    $env:PATH = $savedPath
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
    Write-Host "  the sample recorded $waits waits"
}

# ---------------------------------------------------------------------------------------------
# 7. What was built.
# ---------------------------------------------------------------------------------------------
Write-Host ''
foreach ($file in @((Join-Path $Out 'mock\amdhip64.dll'), (Join-Path $Out 'amdhip64.dll'),
        (Join-Path $Out 'amdhip64.lib'), (Join-Path $Out 'test_hip_mock.exe'),
        (Join-Path $Out 'mock\vadd.exe'))) {
    if (-not (Test-Path $file)) { continue }
    $item = Get-Item $file
    $label = $item.FullName.Substring($Out.Length).TrimStart('\')
    '{0,9}  {1,-26}  sha256 {2}' -f $item.Length, $label, (Get-FileHash -Algorithm SHA256 $item.FullName).Hash
}
