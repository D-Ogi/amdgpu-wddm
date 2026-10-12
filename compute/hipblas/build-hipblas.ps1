# Builds bc250hipblas.dll, its import library, the host test of the matrix multiply and the
# device test program (M16, route B, step 3). Design docs/design/m16-hip-route-b.md section
# 4.12; the route and the ten names are compute/hipblas/README.md.
#
# Nothing is installed and nothing is written to drive C:. The whole build is the portable
# AMDGPU clang of toolchain\llvm-amdgpu-22.1.8: unlike layer 1 and layer 2, this component is
# HIP device code, so MSVC cannot compile it. MSVC is still needed for its environment (the
# host pass includes <cmath> and the link needs the Windows SDK libraries), which the script
# takes from vcvars64 as build-runtime.ps1 does.
#
#   pwsh bc250-win\compute\hipblas\build-hipblas.ps1 -HipLib P:\bc-250\scratch\build\m16-step3
#
# -HipLib names the directory of layer 2's output: amdhip64.lib, and, in its mock\ subdirectory,
# the mock build of amdhip64.dll. Without it the DLL cannot be linked, because every kernel
# launch of this library goes through our HIP runtime.
#
# -SkipMockRun leaves out the run of the device test against the mock backend. The run proves
# that every refusal refuses before a dispatch is built, so leave it in unless the mock DLL is
# missing.

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
    [string]$Out = '',
    [string]$ClangBin = '',
    [string]$HipLib = '',
    [string]$MockDir = '',
    [string]$OffloadArch = 'gfx1013',
    [switch]$SkipMockRun
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$hipblas = Join-Path $repo 'compute\hipblas'
$hipInclude = Join-Path $repo 'compute\hip\include'
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\m16-hipblas' }
if (-not $ClangBin) { $ClangBin = Join-Path $Root 'toolchain\llvm-amdgpu-22.1.8\mingw64\bin' }
if (-not $MockDir -and $HipLib) { $MockDir = Join-Path $HipLib 'mock' }

$clang = Join-Path $ClangBin 'clang++.exe'
$dlltool = Join-Path $ClangBin 'llvm-dlltool.exe'
if (-not (Test-Path $clang)) { throw "clang++.exe not found at $clang" }

New-Item -ItemType Directory -Force $Out | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Out 'mock') | Out-Null
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$def = Join-Path $hipblas 'bc250hipblas.def'
$header = Join-Path $hipInclude 'hipblas\hipblas.h'

# ---------------------------------------------------------------------------------------------
# 1. The export list: the module definition file and the interface header must agree.
# ---------------------------------------------------------------------------------------------
$expectedExports = 11
$defNames = Get-Content $def |
    Where-Object { $_ -match '^[A-Za-z_]' -and $_ -notmatch '^(LIBRARY|EXPORTS)' } |
    ForEach-Object { $_.Trim() }
Write-Host "  bc250hipblas.def holds $($defNames.Count) names"
if ($defNames.Count -ne $expectedExports) {
    throw "bc250hipblas.def holds $($defNames.Count) names, and the design states $expectedExports"
}
$headerText = Get-Content $header -Raw
foreach ($name in $defNames) {
    if ($headerText -notmatch "(?m)\b$([regex]::Escape($name))\s*\(") {
        throw "the exported name $name has no declaration in hipblas/hipblas.h"
    }
}
Write-Host '  every exported name is declared in hipblas/hipblas.h'

# ---------------------------------------------------------------------------------------------
# 2. The MSVC environment. MEASURED (the evidence README of the dp4a work says the same for its
#    section 3): clang compiles the host pass of a .hip file as well, and that pass includes
#    <cmath>, so the MSVC and Windows SDK include and library paths have to be in the
#    environment even though cl.exe is never started.
# ---------------------------------------------------------------------------------------------
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
$savedPath = $env:PATH
& cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^(INCLUDE|LIB|LIBPATH|UCRTVersion|WindowsSdkDir|WindowsSDKVersion)=(.*)$') {
        Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
    }
    if ($_ -match '^Path=(.*)$') { $env:PATH = "$ClangBin;$($Matches[1])" }
}
$msvcRoot = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$dumpbin = Join-Path $msvcRoot.FullName 'bin\Hostx64\x64\dumpbin.exe'

function Invoke-Clang([string[]]$Arguments, [string]$What) {
    $spoken = & $clang @Arguments 2>&1
    $code = $LASTEXITCODE
    $spoken | ForEach-Object { Write-Host "  $_" }
    if ($code -ne 0) {
        $env:PATH = $savedPath
        throw "clang failed for $What ($code)"
    }
}

# The flags every compilation of this component shares. -nogpuinc and -nogpulib: the kernels of
# this library call no function of the ROCm device library (every conversion is in our own
# hip_fp16.h and hip_bf16.h), so the device pass needs neither clang's HIP include tree nor the
# bitcode. The same two flags build the vadd sample of layer 2.
$common = @('--target=x86_64-pc-windows-msvc', '-O2', '-std=c++17', '-Wall', '-Wextra',
    '-Werror', "-I$hipInclude", "-I$(Join-Path $hipblas 'src')")
$hipFlags = @('-x', 'hip', "--offload-arch=$OffloadArch", '-nogpuinc', '-nogpulib',
    '-fno-gpu-rdc')

# ---------------------------------------------------------------------------------------------
# 3. The host test: the argument rules, and the kernel's own tile phases on the CPU against an
#    independent reference. Compiled for the host only, which is why it is not -x hip.
# ---------------------------------------------------------------------------------------------
$coreTest = Join-Path $Out 'test_gemm_core.exe'
Invoke-Clang ($common + @('-o', $coreTest, (Join-Path $hipblas 'tests\host\test_gemm_core.cpp'))) 'test_gemm_core'
& $coreTest
if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "test_gemm_core failed ($LASTEXITCODE)" }

# ---------------------------------------------------------------------------------------------
# 4. The library itself.
# ---------------------------------------------------------------------------------------------
if (-not $HipLib -or -not (Test-Path (Join-Path $HipLib 'amdhip64.lib'))) {
    $env:PATH = $savedPath
    throw "-HipLib must name the output directory of compute\hip\build-runtime.ps1 (no amdhip64.lib in '$HipLib')"
}
$dll = Join-Path $Out 'bc250hipblas.dll'
$implibFromLink = Join-Path $Out 'bc250hipblas.link.lib'
Invoke-Clang ($common + $hipFlags + @('-DBC250_HIPBLAS_BUILD_DLL=1', '-shared',
    '-o', $dll, (Join-Path $hipblas 'src\bc250hipblas.hip'),
    "-L$HipLib", '-Xlinker', "/DEF:$def", '-Xlinker', "/IMPLIB:$implibFromLink",
    '-Xlinker', '/Brepro')) 'bc250hipblas.dll'

# The import library comes from the definition file through llvm-dlltool, as amdhip64.lib does:
# the library has to hold the jump thunks as well as the __imp_ symbols, and its name has to be
# exactly bc250hipblas.lib, because hipblas-config.cmake looks for that name in <root>/lib.
$implib = Join-Path $Out 'bc250hipblas.lib'
& $dlltool -m i386:x86-64 -d $def -l $implib -D bc250hipblas.dll
if ($LASTEXITCODE -ne 0) { $env:PATH = $savedPath; throw "llvm-dlltool failed ($LASTEXITCODE)" }
Write-Host "  bc250hipblas.lib $((Get-Item $implib).Length) bytes"

# ---------------------------------------------------------------------------------------------
# 5. The exports of what was built must equal the definition file, name for name.
# ---------------------------------------------------------------------------------------------
if (Test-Path $dumpbin) {
    $found = @()
    foreach ($line in (& $dumpbin /nologo /exports $dll)) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $found += $Matches[1] }
    }
    $missing = $defNames | Where-Object { $found -notcontains $_ }
    $extra = $found | Where-Object { $defNames -notcontains $_ }
    if ($missing) { $env:PATH = $savedPath; throw "bc250hipblas.dll does not export: $($missing -join ', ')" }
    if ($extra) { $env:PATH = $savedPath; throw "bc250hipblas.dll exports more than the definition file: $($extra -join ', ')" }
    Write-Host "  bc250hipblas.dll exports exactly the $($found.Count) names of bc250hipblas.def"

    # The import table must name our own runtime and nothing of AMD.
    $imports = & $dumpbin /nologo /dependents $dll | Select-String -Pattern '\.dll$'
    $imports | ForEach-Object { Write-Host "  imports $($_.Line.Trim())" }
    if (-not ($imports -match 'amdhip64.dll')) {
        $env:PATH = $savedPath; throw 'bc250hipblas.dll does not import amdhip64.dll'
    }
} else {
    Write-Host '  export check skipped: dumpbin.exe not found'
}

# ---------------------------------------------------------------------------------------------
# 6. The device test program. It is the lab arm of part 3B and the mock run below.
# ---------------------------------------------------------------------------------------------
$checkExe = Join-Path $Out 'gemm_check.exe'
# The import library goes to the linker with -Xlinker and not as an input file: with -x hip in
# effect, clang reads every following input as a HIP source and tries to compile the archive.
Invoke-Clang ($common + $hipFlags + @('-o', $checkExe,
    (Join-Path $hipblas 'tests\gemm_check.hip'), "-L$HipLib", '-Xlinker', $implib)) 'gemm_check'

# ---------------------------------------------------------------------------------------------
# 7. The mock run: every refusal must refuse before a dispatch is built, and every accepted
#    call must build exactly one. The mock backend executes no instruction, so the program runs
#    with --no-numeric and the numbers are the lab's job.
# ---------------------------------------------------------------------------------------------
if (-not $SkipMockRun) {
    $mockAmd = Join-Path $MockDir 'amdhip64.dll'
    if (-not (Test-Path $mockAmd)) {
        Write-Host "  the mock run is skipped: no amdhip64.dll in $MockDir"
    } else {
        $mockOut = Join-Path $Out 'mock'
        Copy-Item $mockAmd $mockOut -Force
        Copy-Item $dll $mockOut -Force
        Copy-Item $checkExe $mockOut -Force
        $record = Join-Path $mockOut 'record-gemm.txt'
        if (Test-Path $record) { Remove-Item $record -Force }
        $env:BC250_HIP_MOCK_RECORD = $record
        $spoken = & (Join-Path $mockOut 'gemm_check.exe') '--no-numeric' '--wait-total' '10000' 2>&1
        $code = $LASTEXITCODE
        Remove-Item env:BC250_HIP_MOCK_RECORD
        $spoken | ForEach-Object { Write-Host "  $_" }
        if ($code -ne 0) { $env:PATH = $savedPath; throw "gemm_check against the mock failed ($code)" }
        $expected = 0
        foreach ($line in $spoken) {
            if ($line -match 'expected dispatches (\d+)') { $expected = [int]$Matches[1] }
        }
        if (-not (Test-Path $record)) {
            $env:PATH = $savedPath; throw 'the mock backend wrote no record of the gemm_check run'
        }
        $dispatches = (Get-Content $record | Where-Object { $_ -match '^dispatch ' })
        Write-Host "  the mock recorded $($dispatches.Count) dispatches, the program expected $expected"
        if ($expected -le 0) {
            $env:PATH = $savedPath; throw 'gemm_check printed no expected dispatch count'
        }
        if ($dispatches.Count -ne $expected) {
            $env:PATH = $savedPath
            throw "the mock recorded $($dispatches.Count) dispatches and gemm_check expected ${expected}: a refused call reached the device, or an accepted one did not"
        }
        # Every dispatch of this library is one of the two GEMM kernels, in a workgroup of
        # 16 x 16 work items. A dispatch with another block shape is not ours.
        $wrongBlock = $dispatches | Where-Object { $_ -notmatch 'block=16,16,1' }
        if ($wrongBlock) {
            $env:PATH = $savedPath
            throw "a dispatch does not carry the 16x16 workgroup of the GEMM kernel: $($wrongBlock[0])"
        }
        $kernels = @()
        foreach ($line in $dispatches) {
            if ($line -match 'kernel=(\S+)') { $kernels += $Matches[1] }
        }
        $names = $kernels | Sort-Object -Unique
        Write-Host "  the dispatched kernels: $($names -join ', ')"
    }
}

$env:PATH = $savedPath

# ---------------------------------------------------------------------------------------------
# 8. What was built.
# ---------------------------------------------------------------------------------------------
Write-Host ''
foreach ($file in @($dll, $implib, $coreTest, $checkExe, (Join-Path $Out 'mock\gemm_check.exe'))) {
    if (-not (Test-Path $file)) { continue }
    $item = Get-Item $file
    $label = $item.FullName.Substring($Out.Length).TrimStart('\')
    '{0,9}  {1,-26}  sha256 {2}' -f $item.Length, $label, (Get-FileHash -Algorithm SHA256 $item.FullName).Hash
}
