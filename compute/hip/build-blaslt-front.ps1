# build-blaslt-front.ps1 - libhipblaslt.dll, our front of AMD's hipBLASLt (compute\hip\blaslt).
#
# TheRock's libhipblaslt.dll for gfx1010 has no kernel library, so torch._int_mm (i8 x i8 -> i32)
# fails in hipblasLtMatmulAlgoGetHeuristic. The front serves that one case through rocblas_gemm_ex
# and forwards everything else to AMD's DLL, renamed libhipblaslt_amd.dll by the attach step
# (blaslt\README.md). -AmdBlasLt names the AMD libhipblaslt.dll the front is built for.
#
# Steps, in order:
#   1. libhipblaslt.def from AMD's export table: every name and ordinal of AMD's DLL, as a PE
#      forwarder to libhipblaslt_amd.<name>, except the nine names that hipblaslt_front.cpp
#      implements; plus bc250lt_get_counters. AMD's DLL must forward nothing itself.
#   2. The import library of libhipblaslt_amd.dll (blaslt\libhipblaslt_amd.def).
#   3. The front, /MT, with its PDB.
#   4. test_blaslt_front: the front beside a stand-in for AMD's DLL and a processor rocblas.dll;
#      the numbers of i8 x i8 -> i32 against a processor reference, real first, the served-case
#      list, the guard against calls from inside rocBLAS, the counters, BC250_BLASLT_FRONT=0.
#   5. The exports of the front: AMD's names with AMD's ordinals, each forwarder to the same name of
#      libhipblaslt_amd, the nine wrapped names not forwarded, and bc250lt_get_counters.
#   6. blaslt-front.json: AMD's DLL (SHA-256, export count), the served names, the front's SHA-256.
#
# Outputs under -Out: libhipblaslt.dll, libhipblaslt.pdb, libhipblaslt.def, blaslt-front.json,
# host-test\ (the front with the two stand-ins) and test_blaslt_front.exe.
param(
    [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl',
    [string]$MsvcReason = '',
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
    [Parameter(Mandatory = $true)][string]$AmdBlasLt,
    [switch]$Lab
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\hip-blaslt-front' }
$AmdBlasLt = (Resolve-Path $AmdBlasLt).Path
$hip = Join-Path $repo 'compute\hip'
$blaslt = Join-Path $hip 'blaslt'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$msvcCl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
$libExe = Join-Path $msvc.FullName 'bin\Hostx64\x64\lib.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$cl = Resolve-Bc250UmdCompiler -Root $Root -Compiler $Compiler -MsvcReason $MsvcReason -Out $Out -Arch 'x64' -MsvcPath $msvcCl
# Compiler temporaries stay off drive C: and inside this build's output, not another release row.
$env:TEMP = Join-Path $Out 'tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$msvcIncludes = @("/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared")
$includes = $msvcIncludes + @("/I$blaslt")
$libpaths = @('/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64",
    "/LIBPATH:$sdkLib\um\x64")
$warn = @('/nologo', '/W4', '/WX', '/O2', '/Brepro', '/MT', '/std:c++17', '/EHsc', '/D_CRT_SECURE_NO_WARNINGS')
$warn += @(if ($Lab) { '/DAMDGPU_WDDM_LAB_FLAVOUR=1' } else { '/DNDEBUG' })

function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    $spoken = @()
    Invoke-Bc250SdkCompiler -Exe $cl -Arguments $Arguments -Roots @($repo) | ForEach-Object {
        $spoken += "$_"
        if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.(c|cpp)$|^\s+Creating library') { Write-Host "  $_" }
    }
    if ($LASTEXITCODE -ne 0) { throw "compile failed for $What ($LASTEXITCODE)" }
    foreach ($line in $spoken) {
        if ($line -match 'LNK4098') { throw "the link of $What mixes two C runtimes (LNK4098): $line" }
    }
}

# Rows of dumpbin /exports: ordinal, name and, for a forwarder, its target.
function Read-Exports([string]$Path) {
    $rows = @()
    foreach ($line in (& $dumpbin /nologo /exports $Path)) {
        # An export of a .def "name=internal" line reads "name = internal".
        if ($line -match '^\s+(\d+)\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)(\s+=\s+\S+)?\s*$') {
            $rows += [pscustomobject]@{ ordinal = [int]$Matches[1]; name = $Matches[2]; forward = '' }
        } elseif ($line -match '^\s+(\d+)\s+[0-9A-Fa-f]+\s+(\S+)\s+\(forwarded to (\S+)\)\s*$') {
            $rows += [pscustomobject]@{ ordinal = [int]$Matches[1]; name = $Matches[2]; forward = $Matches[3] }
        }
    }
    return , $rows
}

$served = @('hipblasLtDestroy', 'hipblasLtMatmulDescCreate', 'hipblasLtMatmulDescDestroy',
    'hipblasLtMatmulDescSetAttribute', 'hipblasLtMatrixLayoutCreate', 'hipblasLtMatrixLayoutDestroy',
    'hipblasLtMatrixLayoutSetAttribute', 'hipblasLtMatmulAlgoGetHeuristic', 'hipblasLtMatmul')

# ---------------------------------------------------------------------------------------------
# 1. The definition file of the front, from AMD's export table.
# ---------------------------------------------------------------------------------------------
$amdRows = Read-Exports $AmdBlasLt
if ($amdRows.Count -lt 100) { throw "$AmdBlasLt exports $($amdRows.Count) names; that is not AMD's hipBLASLt" }
$amdForwards = @($amdRows | Where-Object { $_.forward })
if ($amdForwards) { throw "$AmdBlasLt forwards $($amdForwards.Count) names itself; the front cannot forward a forwarder" }
$amdNames = @($amdRows | ForEach-Object name)
$absent = @($served | Where-Object { $amdNames -notcontains $_ })
if ($absent) { throw "$AmdBlasLt does not export: $($absent -join ', ')" }
$dupes = @($amdNames | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
if ($dupes) { throw "$AmdBlasLt names twice: $($dupes -join ', ')" }
$amdSha = (Get-FileHash $AmdBlasLt -Algorithm SHA256).Hash.ToLower()
$counterOrdinal = ($amdRows | Measure-Object -Property ordinal -Maximum).Maximum + 1
$defLines = @(
    "; libhipblaslt.dll, the hipBLASLt front (build-blaslt-front.ps1 writes this file; do not edit).",
    "; AMD's export table: $AmdBlasLt",
    ";   SHA-256 $amdSha, $($amdRows.Count) names. Each name keeps AMD's ordinal.",
    "; $($amdRows.Count - $served.Count) names forward to libhipblaslt_amd.dll; $($served.Count) are hipblaslt_front.cpp;",
    "; bc250lt_get_counters is ours.",
    'LIBRARY libhipblaslt.dll',
    'EXPORTS'
)
foreach ($r in ($amdRows | Sort-Object ordinal)) {
    if ($served -contains $r.name) { $defLines += "    $($r.name)=bc250lt_$($r.name) @$($r.ordinal)" }
    else { $defLines += "    $($r.name)=libhipblaslt_amd.$($r.name) @$($r.ordinal)" }
}
$defLines += "    bc250lt_get_counters @$counterOrdinal"
$frontDef = Join-Path $Out 'libhipblaslt.def'
[IO.File]::WriteAllLines($frontDef, [string[]]$defLines, (New-Object Text.UTF8Encoding($false)))
Write-Host "  libhipblaslt.def: $($amdRows.Count) names of AMD's table ($($served.Count) served here) and bc250lt_get_counters"

# ---------------------------------------------------------------------------------------------
# 2. The import library of AMD's DLL under its attach name.
# ---------------------------------------------------------------------------------------------
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$amdLib = Join-Path $obj 'libhipblaslt_amd.lib'
& $libExe /nologo /MACHINE:X64 "/DEF:$(Join-Path $blaslt 'libhipblaslt_amd.def')" "/OUT:$amdLib" | ForEach-Object {
    if ($_ -notmatch 'Creating library|^\s*$') { Write-Host "  $_" }
}
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed for libhipblaslt_amd.def ($LASTEXITCODE)" }

# ---------------------------------------------------------------------------------------------
# 3. The front.
# ---------------------------------------------------------------------------------------------
$frontDll = Join-Path $Out 'libhipblaslt.dll'
Invoke-Cl ($warn + @('/Z7', '/clang:-ffile-compilation-dir=.', '/LD', "/Fo$obj\", "/Fe$frontDll") + $includes +
    @((Join-Path $blaslt 'hipblaslt_front.cpp'), '/link', "/DEF:$frontDef", $amdLib, '/DEBUG', '/OPT:REF', '/OPT:ICF',
        '/PDBALTPATH:%_PDB%') + $libpaths) 'libhipblaslt.dll (front)'

# ---------------------------------------------------------------------------------------------
# 4. The host test: the front beside the two stand-ins.
# ---------------------------------------------------------------------------------------------
$testDir = Join-Path $Out 'host-test'
New-Item -ItemType Directory -Force $testDir | Out-Null
$testObj = Join-Path $Out 'obj-test'
New-Item -ItemType Directory -Force $testObj | Out-Null
$hostDir = Join-Path $hip 'tests\host'
Invoke-Cl ($warn + @('/LD', "/Fo$testObj\", "/Fe$(Join-Path $testDir 'libhipblaslt_amd.dll')") + $includes +
    @((Join-Path $hostDir 'blaslt_fake_amd.cpp'), '/link', "/DEF:$(Join-Path $hostDir 'blaslt_fake_amd.def')") + $libpaths) 'stand-in libhipblaslt_amd.dll'
Invoke-Cl ($warn + @('/LD', "/Fo$testObj\", "/Fe$(Join-Path $testDir 'rocblas.dll')") + $includes +
    @((Join-Path $hostDir 'blaslt_fake_rocblas.cpp'), '/link', "/DEF:$(Join-Path $hostDir 'blaslt_fake_rocblas.def')") + $libpaths) 'processor rocblas.dll'
Copy-Item -LiteralPath $frontDll -Destination (Join-Path $testDir 'libhipblaslt.dll') -Force
$testExe = Join-Path $Out 'test_blaslt_front.exe'
Invoke-Cl ($warn + @("/Fo$testObj\", "/Fe$testExe") + $includes + @((Join-Path $hostDir 'test_blaslt_front.cpp'), '/link') +
    $libpaths) 'test_blaslt_front'
$testText = & $testExe $testDir 2>&1 | ForEach-Object { "$_" }
$testCode = $LASTEXITCODE
$testText | Set-Content (Join-Path $Out 'test_blaslt_front.txt') -Encoding utf8
$testText | ForEach-Object { if ($_ -notmatch '^PASS ') { Write-Host "  $_" } }
$passes = @($testText | Where-Object { $_ -match '^PASS ' }).Count
Write-Host "  test_blaslt_front: $passes PASS lines (test_blaslt_front.txt)"
if ($testCode -ne 0) { throw "test_blaslt_front failed ($testCode)" }

# ---------------------------------------------------------------------------------------------
# 5. The exports of the front.
# ---------------------------------------------------------------------------------------------
$frontRows = Read-Exports $frontDll
$byName = @{}
foreach ($r in $frontRows) { $byName[$r.name] = $r }
$problems = @()
foreach ($r in $amdRows) {
    $f = $byName[$r.name]
    if (-not $f) { $problems += "missing $($r.name)"; continue }
    if ($f.ordinal -ne $r.ordinal) { $problems += "$($r.name): ordinal $($f.ordinal), AMD's is $($r.ordinal)" }
    if ($served -contains $r.name) {
        if ($f.forward) { $problems += "$($r.name) is served here but forwards to $($f.forward)" }
    } elseif ($f.forward -ne "libhipblaslt_amd.$($r.name)") {
        $problems += "$($r.name) forwards to '$($f.forward)', not to libhipblaslt_amd.$($r.name)"
    }
}
$extra = @($frontRows | Where-Object { $amdNames -notcontains $_.name -and $_.name -ne 'bc250lt_get_counters' } | ForEach-Object name)
if ($extra) { $problems += "exports beyond AMD's table: $($extra -join ', ')" }
if (-not $byName['bc250lt_get_counters'] -or $byName['bc250lt_get_counters'].forward) { $problems += 'bc250lt_get_counters is missing' }
if ($problems) { throw "the exports of $frontDll are wrong:`n  $($problems -join "`n  ")" }
Write-Host "  $frontDll exports AMD's $($amdRows.Count) names at AMD's ordinals ($($amdRows.Count - $served.Count) forwarders) and bc250lt_get_counters"

# ---------------------------------------------------------------------------------------------
# 6. The record.
# ---------------------------------------------------------------------------------------------
$frontSha = (Get-FileHash $frontDll -Algorithm SHA256).Hash.ToLower()
[ordered]@{
    amd_blaslt       = [ordered]@{ path = $AmdBlasLt; sha256 = $amdSha; exports = $amdRows.Count }
    served_names     = $served
    extra_names      = @('bc250lt_get_counters')
    front            = [ordered]@{ path = $frontDll; sha256 = $frontSha }
    host_test        = [ordered]@{ pass_lines = $passes; exit = $testCode }
    attach_name      = 'libhipblaslt_amd.dll'
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'blaslt-front.json') -Encoding utf8
foreach ($f in @($frontDll, (Join-Path $Out 'libhipblaslt.pdb'))) {
    if (Test-Path $f) { Write-Host "  $((Get-FileHash $f -Algorithm SHA256).Hash.ToLower())  $f" }
}
