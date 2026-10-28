# Builds the two shader edge-case clients, vkshaderedge-x64.exe (Vulkan) and d3d11shaderedge-x64.exe (D3D11), with
# clang-cl. Headers and import libraries come from the SDK NuGet packages under -Kits, the Vulkan headers from
# -VulkanInclude, the MSVC headers and libraries from the installed Visual Studio. No cl.exe step, no fallback.
#
# The compiled programs are tracked in gen\ as C headers, together with the record gen\shaders.txt (compiler, its
# SHA-256 and the command of every header), as tools\win\conformance-clients does. A normal build only reads them,
# so it needs none of the shader compilers. Three switches use the compilers:
#   -Regenerate   compile every program again into gen\ and rewrite gen\shaders.txt (commit the result)
#   -Check        compile every program into the output directory and compare with gen\, line by line
# Both need -Dxc (DXC with SPIR-V code generation; the DXC of the Windows SDK has none), -Fxc2 and -Fxc2No64 (the
# FXC2 tool builds, which are never shipped: README.md, "FXC2"). FXC is the fxc.exe of -Kits.
#
# The build ends with --help and usage checks, and on a PC with a GPU with a self-test of both clients against any
# device and their negative controls. -SkipSelfTest leaves those out.
#
#   pwsh tools\win\shader-edge\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
#   pwsh tools\win\shader-edge\build.ps1 -Kits ... -Check -Dxc <dxc.exe> -Fxc2 <fxc2.exe> -Fxc2No64 <fxc2.exe>
param(
    [Parameter(Mandatory)][string]$Kits,
    [string]$Out = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path -Parent (Split-Path -Parent $Kits) })\scratch\build\shader-edge",
    [string]$KitVersion = '10.0.26100.0',
    [string]$VulkanInclude = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path -Parent (Split-Path -Parent $Kits) })\scratch\m15\vkd3d\khronos\Vulkan-Headers\include",
    [string]$Clang = "$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { Split-Path -Parent (Split-Path -Parent $Kits) })\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe",
    [string]$Dxc,
    [string]$Fxc2,
    [string]$Fxc2No64,
    [switch]$Regenerate,
    [switch]$Check,
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gen = Join-Path $here 'gen'
$root = if ($env:BC250_ROOT) { [IO.Path]::GetFullPath($env:BC250_ROOT) } else { [IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $Kits))) }
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$fxc = Join-Path $sdk "bin\$KitVersion\x64\fxc.exe"
if ($Regenerate -and $Check) { throw '-Regenerate and -Check are alternatives' }
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP = Join-Path $root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# ---- the programs ------------------------------------------------------------------------------------------------
# One row per tracked header: compiler, header name (gen\<name>.h, variable g_<name>), source, arguments.
$vkCommon = @('-nologo', '-spirv', '-T', 'cs_6_6', '-HV', '2021', '-fspv-target-env=vulkan1.2')
$programs = @(
    @('dxc', 'vk_atomic_exact_ssbo', 'vk\shaders\atomics.hlsl', ($vkCommon + @('-E', 'exact_main'))),
    @('dxc', 'vk_atomic_exact_bda', 'vk\shaders\atomics.hlsl', ($vkCommon + @('-E', 'exact_main', '-D', 'ROUTE_BDA'))),
    @('dxc', 'vk_atomic_mp_ssbo', 'vk\shaders\atomics.hlsl', ($vkCommon + @('-E', 'mp_main'))),
    @('dxc', 'vk_atomic_mp_bda', 'vk\shaders\atomics.hlsl', ($vkCommon + @('-E', 'mp_main', '-D', 'ROUTE_BDA'))),
    @('dxc', 'vk_clock_subgroup', 'vk\shaders\clock.hlsl', ($vkCommon + @('-E', 'clock_main', '-D', 'SCOPE=3'))),
    @('dxc', 'vk_clock_device', 'vk\shaders\clock.hlsl', ($vkCommon + @('-E', 'clock_main', '-D', 'SCOPE=1')))
)
foreach ($n in 3, 5, 7) {
    foreach ($v in @(@(0, 'scalar'), @(1, 'vector'))) {
        $programs += , @('dxc', "vk_half$($n)_$($v[1])", 'vk\shaders\half_bda.hlsl',
            ($vkCommon + @('-enable-16bit-types', '-E', 'half_main', '-D', "N=$n", '-D', "VECTOR=$($v[0])")))
    }
}
foreach ($c in 'fxc', 'fxc2') {
    foreach ($e in 'fu', 'loop', 'switch', 'indexed', 'bits') {
        $programs += , @($c, "$($e)_$c", 'd3d11\shaders\corpus_cs.hlsl', @('-nologo', '-T', 'cs_5_0', '-E', "$($e)_main"))
    }
}
foreach ($c in 'fxc', 'fxc2', 'fxc2no64') {
    $tess = 'd3d11\shaders\tess_cpid.hlsl'
    $programs += , @($c, "tess_vs_$c", $tess, @('-nologo', '-T', 'vs_5_0', '-E', 'vs_main'))
    $programs += , @($c, "tess_hs_array_$c", $tess, @('-nologo', '-T', 'hs_5_0', '-E', 'hs_main', '-D', 'HS_DIRECT=0'))
    $programs += , @($c, "tess_hs_direct_$c", $tess, @('-nologo', '-T', 'hs_5_0', '-E', 'hs_main', '-D', 'HS_DIRECT=1'))
    $programs += , @($c, "tess_ds_$c", $tess, @('-nologo', '-T', 'ds_5_0', '-E', 'ds_main'))
    $programs += , @($c, "tess_ps_$c", $tess, @('-nologo', '-T', 'ps_5_0', '-E', 'ps_main'))
}

function Get-Tool([string]$name) {
    $path = switch ($name) { 'dxc' { $Dxc } 'fxc' { $fxc } 'fxc2' { $Fxc2 } 'fxc2no64' { $Fxc2No64 } }
    if (-not $path -or -not (Test-Path -LiteralPath $path)) { throw "compiler $name not found: '$path' (see the header of build.ps1)" }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-ToolVersion([string]$name, [string]$path) {
    switch ($name) {
        'dxc' { return ((& $path --version) | Out-String).Trim() }
        'fxc' { return "Windows SDK $KitVersion" }
        default {
            $v = Join-Path (Split-Path -Parent $path) 'fxc2.version'
            if (-not (Test-Path -LiteralPath $v)) { throw "$name has no fxc2.version next to it" }
            return ((Get-Content -LiteralPath $v) -join '; ')
        }
    }
}

function Invoke-Program($row, [string]$header) {
    $tool = Get-Tool $row[0]
    $argv = @($row[3]) + @('-Vn', "g_$($row[1])", '-Fh', $header, (Join-Path $here $row[2]))
    $log = & $tool @argv 2>&1
    if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host "    $_" }; throw "$($row[0]) failed for $($row[1]) ($LASTEXITCODE)" }
}

if ($Regenerate -or $Check) {
    $target = if ($Regenerate) { $gen } else { Join-Path $Out 'gen-check' }
    New-Item -ItemType Directory -Force $target | Out-Null
    $record = @()
    foreach ($name in 'dxc', 'fxc', 'fxc2', 'fxc2no64') {
        $path = Get-Tool $name
        $shown = $path
        if ($shown.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { $shown = '<BC250_ROOT>' + $shown.Substring($root.Length) }
        $record += "compiler $name $shown"
        $record += "compiler $name sha256 $((Get-FileHash -LiteralPath $path).Hash)"
        $record += "compiler $name version $(Get-ToolVersion $name $path)"
    }
    foreach ($row in $programs) {
        Invoke-Program $row (Join-Path $target "$($row[1]).h")
        $record += "$($row[0]) $(@($row[3]) -join ' ') -Vn g_$($row[1]) -Fh gen\$($row[1]).h $($row[2])"
    }
    if ($Regenerate) {
        $record | Set-Content -LiteralPath (Join-Path $gen 'shaders.txt') -Encoding utf8NoBOM
        Write-Host "  regenerated $($programs.Count) headers in gen and gen\shaders.txt"
    } else {
        $tracked = @(Get-Content -LiteralPath (Join-Path $gen 'shaders.txt'))
        $failures = @()
        foreach ($line in $record | Where-Object { $_ -like 'compiler * sha256 *' }) {
            if ($tracked -notcontains $line) { $failures += "compiler differs from the record: $line" }
        }
        foreach ($row in $programs) {
            $a = @(Get-Content -LiteralPath (Join-Path $gen "$($row[1]).h"))
            $b = @(Get-Content -LiteralPath (Join-Path $target "$($row[1]).h"))
            if (Compare-Object $a $b -SyncWindow 0) { $failures += "gen\$($row[1]).h differs from the rebuild" }
        }
        $headers = @(Get-ChildItem -LiteralPath $gen -Filter '*.h' -File)
        if ($headers.Count -ne $programs.Count) { $failures += "gen holds $($headers.Count) program headers, the table $($programs.Count)" }
        if ($failures) { $failures | ForEach-Object { Write-Host "FAIL $_" }; throw "shader headers do not match ($($failures.Count))" }
        Write-Host "  $($programs.Count) of $($programs.Count) tracked headers reproduce from their compilers"
    }
}
foreach ($row in $programs) {
    if (-not (Test-Path -LiteralPath (Join-Path $gen "$($row[1]).h"))) { throw "gen\$($row[1]).h is missing; build with -Regenerate" }
}

# ---- the clients ------------------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Clang)) { throw "Required clang-cl not found: $Clang" }
$clangOut = @(& $Clang --version)
$clangVersion = $clangOut[0]
if ($LASTEXITCODE -or $clangVersion -notmatch 'clang version') { throw 'clang-cl version query failed' }
Write-Host "  user-mode compiler $Clang"
Write-Host "  $clangVersion"
if (-not (Test-Path -LiteralPath (Join-Path $VulkanInclude 'vulkan\vk_icd.h'))) {
    throw "Vulkan headers not found: $VulkanInclude (expected vulkan\vk_icd.h)"
}
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$sys = @("/imsvc$(Join-Path $msvc.FullName 'include')", "/imsvc$sdk\Include\$KitVersion\ucrt",
    "/imsvc$sdk\Include\$KitVersion\um", "/imsvc$sdk\Include\$KitVersion\shared")
$libs = @("/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")
$flags = @('-fuse-ld=lld', '--target=x86_64-pc-windows-msvc', '-march=x86-64-v3', '-mtune=znver2', '/nologo', '/W4', '/WX',
    '/O2', '/MT', '/Brepro', "/I$gen",
    # Vulkan's {sType} aggregate idiom value-initializes every remaining field to zero (as tools\build\umd-compiler.ps1).
    '/clang:-Wno-missing-field-initializers')
$env:INCLUDE = ''; $env:LIB = ''
$clients = @(
    @{ Exe = 'vkshaderedge-x64.exe'; Src = 'vk\vkshaderedge.c'; Lang = @('/TC', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', "/imsvc$VulkanInclude");
       Libs = @('kernel32.lib', 'psapi.lib', 'bcrypt.lib') },
    @{ Exe = 'd3d11shaderedge-x64.exe'; Src = 'd3d11\d3d11shaderedge.cpp'; Lang = @('/TP', '/std:c++17', '/EHsc', '/DUNICODE', '/D_UNICODE');
       Libs = @('d3d11.lib', 'dxgi.lib', 'kernel32.lib', 'psapi.lib', 'bcrypt.lib') }
)
foreach ($c in $clients) {
    $exe = Join-Path $Out $c.Exe
    # A reviewed artifact is never lost to a rebuild: the existing binary is kept under retained\ by its full hash.
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        $keep = Join-Path $Out "retained\$([IO.Path]::GetFileNameWithoutExtension($c.Exe))-$hash.exe"
        if (-not (Test-Path -LiteralPath $keep)) {
            New-Item -ItemType Directory -Force (Split-Path -Parent $keep) | Out-Null
            Copy-Item -LiteralPath $exe -Destination $keep
        }
    }
    Write-Host "  clang-cl $($c.Exe)"
    & $Clang @($flags + $c.Lang + $sys + @("/Fo$(Join-Path $Out ([IO.Path]::ChangeExtension($c.Exe, '.obj')))", "/Fe$exe",
        (Join-Path $here $c.Src), '/link', '/Brepro') + $libs + $c.Libs) |
        ForEach-Object { if ($_ -notmatch '^\s*$|^\S+\.(c|cpp)$') { Write-Host "    $_" } }
    if ($LASTEXITCODE -ne 0) { throw "clang-cl failed for $($c.Exe) ($LASTEXITCODE)" }
    & $exe --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "help check failed for $($c.Exe) ($LASTEXITCODE)" }
    & $exe --bad-option 2>$null | Out-Null
    if ($LASTEXITCODE -ne 2) { throw "usage check failed for $($c.Exe) ($LASTEXITCODE)" }
}
@{ compiler = 'clang-cl'; path = $Clang; version = $clangVersion } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $Out 'compiler.json') -Encoding utf8

# ---- self-test against any device of this PC ---------------------------------------------------------------------
# A development PC without a GPU is no reason to refuse the artifacts; the build says what it could not run.
if ($SkipSelfTest) {
    Write-Host '  self-test skipped on request'
} else {
    $vk = Join-Path $Out 'vkshaderedge-x64.exe'
    & $vk --list-devices | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  Vulkan self-test skipped: this PC offers no Vulkan device'
    } else {
        Write-Host '  self-test vkshaderedge --any-device'
        & $vk --any-device | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "vkshaderedge self-test failed ($LASTEXITCODE)" }
        & $vk --any-device --negative-control | Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 1) { throw "vkshaderedge negative control did not fail as required ($LASTEXITCODE)" }
    }
    $dx = Join-Path $Out 'd3d11shaderedge-x64.exe'
    foreach ($device in @(@('--warp'), @('--any-adapter'))) {
        Write-Host "  self-test d3d11shaderedge $($device -join ' ')"
        & $dx @device | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { throw "d3d11shaderedge self-test failed on $device ($LASTEXITCODE)" }
        & $dx @device --negative-control | Select-Object -Last 2 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 1) { throw "d3d11shaderedge negative control did not fail as required on $device ($LASTEXITCODE)" }
    }
    # The FXC2 bytecode without patch 64 runs on WARP only here: on a hardware device it can remove the device.
    # tess-direct must pass. tess-array is the hostile hull shader (x0[vOutputControlPointID + 0]): WARP draws
    # nothing with it and reports no error (README.md, "Reference results"), so the build shows that result and
    # does not gate on it.
    Write-Host '  self-test d3d11shaderedge --warp --with-no64 --case tess-direct'
    & $dx --warp --with-no64 --case tess-direct | Select-Object -Last 4 | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw "d3d11shaderedge no64 tess-direct self-test failed on WARP ($LASTEXITCODE)" }
    Write-Host '  probe d3d11shaderedge --warp --with-no64 --case tess-array (not a gate)'
    & $dx --warp --with-no64 --case tess-array | Select-String 'fxc2no64' | ForEach-Object { Write-Host "    $_" }
    Write-Host "    exit $LASTEXITCODE"
}

Get-ChildItem $Out -Filter '*shaderedge-x64.exe' | Sort-Object Name | ForEach-Object {
    '{0,9}  {1}  sha256 {2}' -f $_.Length, $_.Name, (Get-FileHash -LiteralPath $_.FullName).Hash }
