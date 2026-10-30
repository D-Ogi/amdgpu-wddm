# build-amd-abi.ps1 - amdhip64_7.dll, the AMD ABI face of the HIP runtime (runtime\amd_abi.cpp).
#
# The ROCm user libraries of TheRock (torch_hip, rocBLAS, hipBLASLt, MIOpen and the others) import
# their HIP names from amdhip64_7.dll and were compiled against AMD's hip_runtime_api.h. This
# script builds the same runtime as build-runtime.ps1 with BC250_HIP_AMD_ABI=1, plus the AMD face:
#
#   - amd_abi.cpp, the one file compiled against AMD's header (-AmdInclude), which implements the
#     names whose AMD meaning differs from ours and the names that only the ROCm libraries import;
#   - hip_amd_face.cpp and hip_kpack.cpp, compiled against our header, behind amd_face.h;
#   - amdhip64_7.def, the export list.
#
# Gates, in order:
#   1. test_amd_abi_agreement: the declared names that the DLL exports unchanged have the same type,
#      and every value that crosses them the same number, under both headers.
#   2. The exports of every DLL built equal amdhip64_7.def, and every name of the import census
#      (-Census, imp-hip.tsv) is exported.
#   3. amd-headers.json records the SHA-256 of every AMD header that amd_abi.cpp included.
#
# Outputs under -Out: mock\amdhip64_7.dll (the runtime over tests\host\hipmock_backend.c with the
# production loader, for a machine with no BC-250) and amdhip64_7.dll (the runtime over layer 1),
# both /MT, so neither needs a Visual C runtime beside it.
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
    [Parameter(Mandatory = $true)][string]$AmdInclude,
    [string]$Census = '',
    [string]$Bc250hsaLib = '',
    [switch]$SkipProduct,
    [switch]$Lab
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\hip-amd-abi' }
$AmdInclude = (Resolve-Path $AmdInclude).Path
if (-not (Test-Path (Join-Path $AmdInclude 'hip\hip_runtime_api.h'))) {
    throw "-AmdInclude holds no hip\hip_runtime_api.h: $AmdInclude"
}
$hip = Join-Path $repo 'compute\hip'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$msvcCl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$dumpbin = Join-Path $msvc.FullName 'bin\Hostx64\x64\dumpbin.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
. (Join-Path $repo 'tools\build\umd-compiler.ps1')
$cl = Resolve-Bc250UmdCompiler -Root $Root -Compiler $Compiler -MsvcReason $MsvcReason -Out $Out -Arch 'x64' -MsvcPath $msvcCl
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

$def = Join-Path $hip 'runtime\amdhip64_7.def'
$defNames = @(Get-Content $def | Where-Object { $_ -match '^[A-Za-z_]' -and $_ -notmatch '^(LIBRARY|EXPORTS)' } |
    ForEach-Object { ($_.Trim() -split '=')[0] })
Write-Host "  amdhip64_7.def holds $($defNames.Count) names"
$commentCount = [regex]::Match((Get-Content $def -Raw), '(?m)^; (\d+) names:').Groups[1].Value
if ("$($defNames.Count)" -ne $commentCount) {
    throw "amdhip64_7.def says $commentCount names in its header and holds $($defNames.Count)"
}
$dupes = @($defNames | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
if ($dupes) { throw "amdhip64_7.def names twice: $($dupes -join ', ')" }

$msvcIncludes = @("/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt",
    "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared")
$includes = $msvcIncludes + @("/I$(Join-Path $hip 'include')", "/I$(Join-Path $hip 'runtime')",
    "/I$(Join-Path $hip 'tests\host')", "/I$(Join-Path $hip 'bc250hsa')")
# amd_abi.cpp and the AMD half of the agreement test see AMD's include directory and not ours:
# our hip\hip_version.h and AMD's have the same name, and the first one found wins. AMD's
# directory is a system root, so its own warnings do not fail /WX.
$amdIncludes = $msvcIncludes + @("/imsvc$AmdInclude", "/I$(Join-Path $hip 'runtime')", "/I$(Join-Path $hip 'tests\host')")
$amdDefines = @('/D__HIP_PLATFORM_AMD__', '/clang:-Wno-deprecated-declarations')
$libpaths = @('/Brepro', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64",
    "/LIBPATH:$sdkLib\um\x64")
$warn = @('/nologo', '/W4', '/WX', '/O2', '/Brepro', '/MT', '/D_CRT_SECURE_NO_WARNINGS', '/DBC250_HIP_BUILD_DLL',
    '/DBC250_HIP_AMD_ABI=1')
$warn += @(if ($Lab) { '/DAMDGPU_WDDM_LAB_FLAVOUR=1' } else { '/DNDEBUG' })

function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    $spoken = @()
    Invoke-Bc250SdkCompiler -Exe $cl -Arguments $Arguments -Roots @($repo) | ForEach-Object {
        $spoken += "$_"
        if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.(c|cpp)$|^\s+Creating library|^Note: including file:') {
            Write-Host "  $_"
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "compile failed for $What ($LASTEXITCODE)" }
    foreach ($line in $spoken) {
        if ($line -match 'LNK4098') { throw "the link of $What mixes two C runtimes (LNK4098): $line" }
    }
    return , $spoken
}

$runtimeSources = @('hip_device.cpp', 'hip_error.cpp', 'hip_event.cpp', 'hip_launch.cpp', 'hip_journal.cpp',
    'hip_kernarg_arena.cpp', 'hip_log.cpp', 'hip_memory.cpp', 'hip_module.cpp', 'hip_occupancy.cpp', 'hip_perf.cpp',
    'hip_stream.cpp', 'hip_kpack.cpp', 'hip_amd_face.cpp', 'dllmain.cpp') | ForEach-Object { Join-Path $hip "runtime\$_" }
$l1 = Join-Path $hip 'bc250hsa'
$loaderSources = @('co_loader.c', 'co_metadata.c', 'co_msgpack.c', 'kernarg.c') | ForEach-Object { Join-Path $l1 $_ }

# ---------------------------------------------------------------------------------------------
# 1. The agreement of the two headers.
# ---------------------------------------------------------------------------------------------
$testObj = Join-Path $Out 'obj-abi-test'
New-Item -ItemType Directory -Force $testObj | Out-Null
$null = Invoke-Cl ($warn + @('/c', '/std:c++17', '/EHsc', "/Fo$testObj\") + $includes +
    @((Join-Path $hip 'tests\host\amd_abi_probe_ours.cpp'), (Join-Path $hip 'tests\host\test_amd_abi_agreement.cpp'))) 'ABI probe (ours)'
$null = Invoke-Cl ($warn + $amdDefines + @('/c', '/std:c++17', '/EHsc', "/Fo$testObj\") + $amdIncludes +
    @((Join-Path $hip 'tests\host\amd_abi_probe_amd.cpp'))) 'ABI probe (AMD)'
$agreeExe = Join-Path $Out 'test_amd_abi_agreement.exe'
$null = Invoke-Cl ($warn + @("/Fe$agreeExe") + @(Get-ChildItem $testObj -Filter '*.obj' | ForEach-Object FullName) +
    @('/link') + $libpaths) 'test_amd_abi_agreement'
& $agreeExe $Out
if ($LASTEXITCODE -ne 0) { throw "test_amd_abi_agreement failed ($LASTEXITCODE)" }

# ---------------------------------------------------------------------------------------------
# 2. amd_abi.cpp, once: it calls our runtime through amd_face.h and the exported names only, so
#    the mock and the product DLL link the same object.
# ---------------------------------------------------------------------------------------------
$faceObj = Join-Path $Out 'obj-face'
New-Item -ItemType Directory -Force $faceObj | Out-Null
$spoken = Invoke-Cl ($warn + $amdDefines + @('/c', '/std:c++17', '/EHsc', '/showIncludes', "/Fo$faceObj\") + $amdIncludes +
    @((Join-Path $hip 'runtime\amd_abi.cpp'))) 'amd_abi.cpp'
$amdHeaders = @($spoken | ForEach-Object {
        if ($_ -match '^Note: including file:\s*(.+)$') { [IO.Path]::GetFullPath($Matches[1].Trim()) }
    } | Where-Object { $_.StartsWith($AmdInclude, [StringComparison]::OrdinalIgnoreCase) } | Sort-Object -Unique)
$headerRecord = [ordered]@{
    amd_include = $AmdInclude
    headers     = @($amdHeaders | ForEach-Object {
            [ordered]@{ path = $_.Substring($AmdInclude.Length + 1); sha256 = (Get-FileHash $_ -Algorithm SHA256).Hash.ToLower() }
        })
}
$headerRecord | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'amd-headers.json') -Encoding utf8
Write-Host "  amd_abi.cpp included $($amdHeaders.Count) AMD headers (amd-headers.json)"
$faceObject = Join-Path $faceObj 'amd_abi.obj'

# ---------------------------------------------------------------------------------------------
# 3. The mock DLL: the runtime over the mock device and the production code object loader, so
#    that the gfx1010 alias, the unbundler and the kernel descriptor checks are the lab's.
# ---------------------------------------------------------------------------------------------
$mockObj = Join-Path $Out 'obj-mock'
New-Item -ItemType Directory -Force $mockObj | Out-Null
$mockDll = Join-Path $Out 'mock\amdhip64_7.dll'
New-Item -ItemType Directory -Force (Split-Path -Parent $mockDll) | Out-Null
$null = Invoke-Cl ($warn + @('/c', '/std:c11', '/DBC250_HIP_MOCK_PRODUCTION_LOADER', "/Fo$mockObj\") + $includes +
    @((Join-Path $hip 'tests\host\hipmock_backend.c')) + $loaderSources) 'mock backend'
$mockC = @('hipmock_backend.obj', 'co_loader.obj', 'co_metadata.obj', 'co_msgpack.obj', 'kernarg.obj') |
    ForEach-Object { Join-Path $mockObj $_ }
$null = Invoke-Cl ($warn + @('/LD', '/std:c++17', '/EHsc', "/Fo$mockObj\", "/Fe$mockDll") + $includes + $runtimeSources +
    $mockC + @($faceObject, '/link', "/DEF:$def") + $libpaths) 'amdhip64_7.dll (mock)'

# ---------------------------------------------------------------------------------------------
# 4. The product DLL over layer 1. Without -Bc250hsaLib the layer-1 sources are compiled here,
#    with the same compiler, and linked as objects.
# ---------------------------------------------------------------------------------------------
$productDll = Join-Path $Out 'amdhip64_7.dll'
if (-not $SkipProduct) {
    $productObj = Join-Path $Out 'obj-product'
    New-Item -ItemType Directory -Force $productObj | Out-Null
    if ($Bc250hsaLib) {
        $layer1 = @((Resolve-Path $Bc250hsaLib).Path)
    } else {
        $l1Obj = Join-Path $Out 'obj-layer1'
        New-Item -ItemType Directory -Force $l1Obj | Out-Null
        $l1Sources = @('status.c', 'co_msgpack.c', 'co_metadata.c', 'co_loader.c', 'kernarg.c', 'pm4_dispatch.c',
            'kmt_device.c', 'kmt_memory.c', 'submit.c')
        $null = Invoke-Cl (@('/nologo', '/W4', '/WX', '/O2', '/MT', '/std:c11', '/Brepro', '/D_CRT_SECURE_NO_WARNINGS',
                '/c', "/Fo$l1Obj\") + $msvcIncludes + @("/I$(Join-Path $hip 'include')", "/I$l1") +
            @($l1Sources | ForEach-Object { Join-Path $l1 $_ })) 'layer 1'
        $layer1 = @($l1Sources | ForEach-Object { Join-Path $l1Obj ([IO.Path]::ChangeExtension($_, 'obj')) })
    }
    $null = Invoke-Cl ($warn + @('/Z7', '/clang:-ffile-compilation-dir=.', '/LD', '/std:c++17', '/EHsc', "/Fo$productObj\",
            "/Fe$productDll") + $includes + $runtimeSources + @($faceObject) + $layer1 +
        @('/link', "/DEF:$def", '/DEBUG', '/OPT:REF', '/OPT:ICF', '/PDBALTPATH:%_PDB%') + $libpaths + @('gdi32.lib')) 'amdhip64_7.dll (bc250hsa)'
}

# ---------------------------------------------------------------------------------------------
# 5. Exports: equal to the definition file, and a superset of the import census.
# ---------------------------------------------------------------------------------------------
$censusNames = @()
if ($Census) {
    $censusNames = @(Import-Csv $Census -Delimiter "`t" | ForEach-Object name)
    $notInDef = @($censusNames | Where-Object { $defNames -notcontains $_ })
    if ($notInDef) { throw "the import census names what amdhip64_7.def does not export: $($notInDef -join ', ')" }
    Write-Host "  amdhip64_7.def exports all $($censusNames.Count) names of the import census"
}
function Assert-Exports([string]$Path) {
    $found = @()
    foreach ($line in (& $dumpbin /nologo /exports $Path)) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $found += $Matches[1] }
    }
    $missing = @($defNames | Where-Object { $found -notcontains $_ })
    $extra = @($found | Where-Object { $defNames -notcontains $_ })
    if ($missing) { throw "$Path does not export: $($missing -join ', ')" }
    if ($extra) { throw "$Path exports more than amdhip64_7.def: $($extra -join ', ')" }
    Write-Host "  $Path exports exactly the $($found.Count) names of amdhip64_7.def"
}
Assert-Exports $mockDll
if (Test-Path $productDll) { Assert-Exports $productDll }

foreach ($f in @($mockDll, $productDll, (Join-Path $Out 'amdhip64_7.pdb'))) {
    if (Test-Path $f) { Write-Host "  $((Get-FileHash $f -Algorithm SHA256).Hash.ToLower())  $f" }
}
