#Requires -Version 7.0
<#
.SYNOPSIS
Configures, builds and installs LLVM the way the lab's LLVM 23.1.2 toolchain was built (docs/build.md).

.DESCRIPTION
Static, Release, /MT, X86 target only, no tools except llvm-config, no tests. This is the LLVM that the
llvmpipe desktop UMD links (build-mesa.ps1 -Config llvmpipe-umd -Llvm <this -Build directory>).

The recorded configuration is evidence/windows/2026-09-24-E26-desktop-resume/llvm23/build-llvm23.cmd, and its
result was checked against that build's CMakeCache.txt. The option table below is the single copy of it in
this repository; docs/build.md quotes it. After configure the script reads the new CMakeCache.txt back and
stops if any recorded value differs, so a silently ignored or renamed option is noticed before a two-hour build.

Mesa finds this LLVM through llvm-config.exe in the BUILD tree (<Build>\bin), not the install prefix: the
recipe builds llvm-config explicitly, and LLVM_BUILD_TOOLS=OFF keeps it out of the install.

.EXAMPLE
pwsh tools\build\build-llvm.ps1 -Build P:\BC-250\scratch\llvm2312-build -Prefix P:\BC-250\toolchain\llvm2312 -Jobs 12

.EXAMPLE
pwsh tools\build\build-llvm.ps1 -ConfigureOnly -Build P:\BC-250\scratch\x\llvm-configure -Prefix P:\BC-250\scratch\x\llvm-prefix
#>
[CmdletBinding()]
param(
    # LLVM monorepo checkout at llvmorg-23.1.2 (default <BC250_ROOT>\ref\llvm-project-23.1.2).
    [string]$Source,
    # Build directory (the recorded one is <BC250_ROOT>\scratch\llvm2312-build).
    [Parameter(Mandatory)][string]$Build,
    # Install prefix (the recorded one is <BC250_ROOT>\toolchain\llvm2312).
    [Parameter(Mandatory)][string]$Prefix,
    [switch]$ConfigureOnly,
    # Parallel jobs for ninja; 0 leaves the choice to ninja. The recorded build used 12.
    [int]$Jobs = 0,
    # cmake.exe and ninja.exe; default: whatever PATH gives after the Visual Studio environment is loaded.
    [string]$CMake,
    [string]$Ninja,
    # Visual Studio installation root; default: vswhere -latest with the x64 C++ tools.
    [string]$VsInstall,
    # TEMP and TMP for the build (default <BC250_ROOT>\scratch\tmp), so that nothing lands on drive C:.
    [string]$Temp,
    # The version CMake must report for the source (CMAKE_PROJECT_VERSION); '' skips the check.
    [string]$ExpectedVersion = '23.1.2',
    # Install over a prefix that already holds files. Off by default: the working toolchain is kept for
    # rollback and comparison until a new one is validated (bc250-win/CLAUDE.md, toolchain freshness).
    [switch]$AllowExistingPrefix
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

# ---- the recipe ----------------------------------------------------------------------------------------------
# Passed on the cmake command line, in the recorded order. CMAKE_MAKE_PROGRAM and CMAKE_INSTALL_PREFIX come
# from the parameters.
$LlvmOptions = [ordered]@{
    CMAKE_BUILD_TYPE           = 'Release'
    CMAKE_C_COMPILER           = 'cl'
    CMAKE_CXX_COMPILER         = 'cl'
    CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
    CMAKE_CXX_FLAGS            = '/utf-8'
    LLVM_TARGETS_TO_BUILD      = 'X86'
    LLVM_ENABLE_ASSERTIONS     = 'OFF'
    LLVM_INCLUDE_UTILS         = 'OFF'
    LLVM_INCLUDE_RUNTIMES      = 'OFF'
    LLVM_INCLUDE_TESTS         = 'OFF'
    LLVM_INCLUDE_EXAMPLES      = 'OFF'
    LLVM_INCLUDE_BENCHMARKS    = 'OFF'
    LLVM_BUILD_TOOLS           = 'OFF'
    LLVM_ENABLE_DIA_SDK        = 'OFF'
    LLVM_ENABLE_ZLIB           = 'OFF'
    LLVM_ENABLE_ZSTD           = 'OFF'
    LLVM_ENABLE_LIBXML2        = 'OFF'
    LLVM_PARALLEL_LINK_JOBS    = '2'
}
# Not passed: LLVM defaults that the recorded cache shows and that the Mesa link depends on (static
# libraries, no RTTI or exceptions, host triple). Checked after configure so a changed default is noticed.
$ExpectedDefaults = [ordered]@{
    BUILD_SHARED_LIBS       = 'OFF'
    LLVM_ENABLE_RTTI        = 'OFF'
    LLVM_ENABLE_EH          = 'OFF'
    LLVM_OPTIMIZED_TABLEGEN = 'OFF'
    LLVM_ENABLE_PROJECTS    = ''
    LLVM_HOST_TRIPLE        = 'x86_64-pc-windows-msvc'
    CMAKE_GENERATOR         = 'Ninja'
}
$NinjaTargets = @('llvm-config', 'install')

# ---- paths ---------------------------------------------------------------------------------------------------
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Get-Bc250Root $repo
if (-not $Source) { $Source = Join-Path $root 'ref\llvm-project-23.1.2' }
if (-not $Temp) { $Temp = Join-Path $root 'scratch\tmp' }
$Source = [IO.Path]::GetFullPath($Source)
$Build = [IO.Path]::GetFullPath($Build)
$Prefix = [IO.Path]::GetFullPath($Prefix)
$llvmDir = Join-Path $Source 'llvm'
if (-not (Test-Path -LiteralPath (Join-Path $llvmDir 'CMakeLists.txt'))) { throw "$Source is not an LLVM monorepo checkout (no llvm\CMakeLists.txt)" }

function Read-CMakeCache([string]$Path) {
    $cache = [Collections.Generic.Dictionary[string, string]]::new()
    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line -match '^([^#/][^:=]*):([A-Z]+)=(.*)$') { $cache[$Matches[1]] = $Matches[3] }
    }
    return , $cache
}

function Test-SamePath([string]$A, [string]$B) {
    return [IO.Path]::GetFullPath($A.Replace('/', '\')).TrimEnd('\') -ieq [IO.Path]::GetFullPath($B.Replace('/', '\')).TrimEnd('\')
}

$cachePath = Join-Path $Build 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath) {
    $old = Read-CMakeCache $cachePath
    if ($old.ContainsKey('CMAKE_HOME_DIRECTORY') -and -not (Test-SamePath $old['CMAKE_HOME_DIRECTORY'] $llvmDir)) {
        throw "$Build was configured from $($old['CMAKE_HOME_DIRECTORY']), not $llvmDir"
    }
    Write-Host "reconfiguring the existing build tree $Build"
}
if (-not $ConfigureOnly -and -not $AllowExistingPrefix -and (Test-Path -LiteralPath $Prefix) -and
    @(Get-ChildItem -LiteralPath $Prefix -Force | Select-Object -First 1).Count) {
    throw "$Prefix already holds files. Install into a new prefix, or pass -AllowExistingPrefix to overwrite it"
}

$recipePath = Join-Path $Build 'recipe.json'
$savedEnv = Save-ProcessEnvironment
try {
    $vs = Import-VsDevEnvironment $VsInstall $Temp
    New-Item -ItemType Directory -Force $Temp, $Build | Out-Null
    $env:TEMP = $Temp
    $env:TMP = $Temp
    $cmakeExe = Resolve-Tool $CMake 'cmake'
    $ninjaExe = Resolve-Tool $Ninja 'ninja'
    if (-not $cmakeExe) { throw 'cmake not found: put it on PATH or pass -CMake' }
    if (-not $ninjaExe) { throw 'ninja not found: put it on PATH or pass -Ninja' }

    $cmakeArgs = @('-S', ($llvmDir -replace '\\', '/'), '-B', ($Build -replace '\\', '/'), '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$($ninjaExe -replace '\\', '/')", "-DCMAKE_INSTALL_PREFIX=$($Prefix -replace '\\', '/')")
    foreach ($k in $LlvmOptions.Keys) { $cmakeArgs += "-D$k=$($LlvmOptions[$k])" }

    $recipe = [ordered]@{
        recipe            = 'llvm'
        schema            = 1
        utc               = [DateTime]::UtcNow.ToString('o')
        status            = 'configuring'
        recipe_identity   = Get-RecipeIdentity $repo @($PSCommandPath, (Join-Path $PSScriptRoot 'common.ps1'))
        bc250_root        = $root
        source            = Get-SourceIdentity $Source
        expected_version  = $ExpectedVersion
        build_dir         = $Build
        prefix            = $Prefix
        cmake_arguments   = $cmakeArgs
        ninja_targets     = $NinjaTargets
        environment       = [ordered]@{
            vs_install          = $vs
            vc_tools_version    = $env:VCToolsVersion
            windows_sdk_version = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { $null }
            temp                = $Temp
        }
        tools             = [ordered]@{
            cmake = [ordered]@{ path = $cmakeExe; version = Get-ToolVersion $cmakeExe }
            ninja = [ordered]@{ path = $ninjaExe; version = Get-ToolVersion $ninjaExe }
            cl    = [ordered]@{ path = (Get-Command cl.exe).Source; version = Get-ClVersion }
            git   = Get-ToolVersion (Resolve-Tool '' 'git')
        }
        gate              = $null
    }
    Write-Recipe $recipePath $recipe

    Invoke-Checked $cmakeExe $cmakeArgs

    # The gate: every recorded value must be what CMake actually stored.
    $cache = Read-CMakeCache $cachePath
    $expect = [ordered]@{}
    foreach ($k in $LlvmOptions.Keys) { $expect[$k] = $LlvmOptions[$k] }
    foreach ($k in $ExpectedDefaults.Keys) { $expect[$k] = $ExpectedDefaults[$k] }
    if ($ExpectedVersion) { $expect['CMAKE_PROJECT_VERSION'] = $ExpectedVersion }
    $mismatches = @()
    foreach ($k in $expect.Keys) {
        $have = if ($cache.ContainsKey($k)) { $cache[$k] } else { '<missing>' }
        # A first configure stores the compiler as the full path cl resolved to; a re-run over the same tree
        # keeps the bare 'cl' from the command line (the recorded cache shows that form). Both mean cl.exe.
        if ($k -in 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER') { $have = [IO.Path]::GetFileNameWithoutExtension($have.Replace('/', '\')) }
        if ($have -ne $expect[$k]) { $mismatches += "$k expected '$($expect[$k])' found '$have'" }
    }
    foreach ($p in @(@('CMAKE_INSTALL_PREFIX', $Prefix), @('CMAKE_MAKE_PROGRAM', $ninjaExe))) {
        if (-not $cache.ContainsKey($p[0]) -or -not (Test-SamePath $cache[$p[0]] $p[1])) { $mismatches += "$($p[0]) is not $($p[1])" }
    }
    $recipe.gate = [ordered]@{ checked = $expect.Count + 2; mismatches = $mismatches }
    $recipe.status = if ($mismatches.Count) { 'configure-gate-failed' } else { 'configured' }
    Write-Recipe $recipePath $recipe
    if ($mismatches.Count) { throw ("CMakeCache.txt differs from the recipe:`n  " + ($mismatches -join "`n  ")) }
    Write-Host "configure gate: $($recipe.gate.checked) values match the recipe"

    if ($ConfigureOnly) { Write-Host "configured only: $Build (recipe: $recipePath)"; return }

    $ninjaArgs = @('-C', $Build)
    if ($Jobs -gt 0) { $ninjaArgs += "-j$Jobs" }
    Invoke-Checked $ninjaExe ($ninjaArgs + $NinjaTargets)

    $llvmConfig = Join-Path $Build 'bin\llvm-config.exe'
    $reported = (Invoke-Capture $llvmConfig @('--version')).Out.Trim()
    if ($ExpectedVersion -and $reported -ne $ExpectedVersion) { throw "llvm-config reports $reported, expected $ExpectedVersion" }
    foreach ($f in 'bin\llvm-tblgen.exe', 'lib\LLVMCore.lib', 'include\llvm\Config\llvm-config.h') {
        if (-not (Test-Path -LiteralPath (Join-Path $Prefix $f))) { throw "install incomplete: $Prefix\$f is missing" }
    }
    $recipe.status = 'installed'
    $recipe['artifacts'] = [ordered]@{
        llvm_config          = $llvmConfig
        llvm_config_version  = $reported
        llvm_config_sha256   = Get-FileSha256 $llvmConfig
        build_mode           = (Invoke-Capture $llvmConfig @('--build-mode')).Out.Trim()
        shared_mode          = (Invoke-Capture $llvmConfig @('--shared-mode')).Out.Trim()
        targets_built        = (Invoke-Capture $llvmConfig @('--targets-built')).Out.Trim()
        installed_files      = @(Get-Content -LiteralPath (Join-Path $Build 'install_manifest.txt')).Count
    }
    Write-Recipe $recipePath $recipe
    Copy-Item -LiteralPath $recipePath -Destination (Join-Path $Prefix 'bc250-recipe.json') -Force
    Write-Host "installed LLVM $reported into $Prefix; Mesa takes -Llvm $Build (llvm-config lives there)"
} finally {
    Restore-ProcessEnvironment $savedEnv
}
