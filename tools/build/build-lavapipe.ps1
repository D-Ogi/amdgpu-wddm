#Requires -Version 7.0
<#
.SYNOPSIS
Builds the lavapipe Vulkan ICD (vulkan_lvp.dll) of the CPU (shell, DXVK, lavapipe) pair for one architecture.

.DESCRIPTION
The CPU route of the D3D11 shell (BD-088 option b) runs DXVK on lavapipe, Mesa's CPU Vulkan driver, instead of the
mesa d3d10umd UMD on llvmpipe, which offers the D3D10 DDI only (FL 10_0). This script is the build half:

  1. build-mesa.ps1 -Config lavapipe (mesa-configs.json) with the LLVM build directory of the architecture;
  2. the machine gate: the DLL header names the requested architecture;
  3. the CRT gate: the DLL imports no C or C++ runtime DLL (static CRT, as the RADV ICD of the shell: an ICD that
     runs inside every application must not take an older msvcp140.dll that a game puts beside its exe);
  4. the export gate: the DLL exports vk_icdGetInstanceProcAddr, which the shell resolves by name;
  5. the artifact directory: vulkan_lvp.dll, its PDB, lvp_icd.json (library_path ".\\vulkan_lvp.dll"), recipe.json
     and SHA256SUMS.

The source tree is the Mesa fork (D-Ogi/mesa-amdgpu-wddm); this script patches nothing. Lavapipe needs LLVM and
glslangValidator (its acceleration-structure shaders). test-lavapipe-smoke.ps1 runs the headless smoke on the result.

.EXAMPLE
pwsh tools\build\build-lavapipe.ps1 -Source P:\BC-250\scratch\lavapipe\mesa-src -Work P:\BC-250\scratch\lavapipe\build -Arch x64

.EXAMPLE
pwsh tools\build\build-lavapipe.ps1 -Source P:\BC-250\scratch\lavapipe\mesa-src -Work P:\BC-250\scratch\lavapipe\build -Arch x86
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Source,
    # Work directory: build-lvp-<arch>\ (meson), lvp-<arch>\ (artifacts), tmp-lvp-<arch>\ and logs\ go under it.
    [Parameter(Mandatory)][string]$Work,
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    # LLVM build directory with bin\llvm-config.exe. Default: <BC250_ROOT>\scratch\llvm2312-build (x64),
    # <BC250_ROOT>\scratch\wow64\llvm2312-x86-build (x86).
    [string]$Llvm,
    [string]$Python,
    [string]$Ninja,
    # The Visual Studio instance that built the LLVM libraries (static libraries must match the compiler).
    [string]$VsInstall,
    [int]$Jobs = 0,
    # Keep an existing build directory and only rebuild (meson setup --reconfigure).
    [switch]$Incremental
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Get-Bc250Root $repo
if (-not $Llvm) {
    $Llvm = if ($Arch -eq 'x86') { Join-Path $root 'scratch\wow64\llvm2312-x86-build' } else { Join-Path $root 'scratch\llvm2312-build' }
}
$Work = [IO.Path]::GetFullPath($Work)
$build = Join-Path $Work "build-lvp-$Arch"
$art = Join-Path $Work "lvp-$Arch"
$temp = Join-Path $Work "tmp-lvp-$Arch"
$logDir = Join-Path $Work 'logs'
New-Item -ItemType Directory -Force $temp, $logDir | Out-Null
$log = Join-Path $logDir "build-lvp-$Arch.log"
$recipe = Join-Path $PSScriptRoot 'build-mesa.ps1'

if (-not $Incremental -and (Test-Path -LiteralPath $build)) { Remove-Item -LiteralPath $build -Recurse -Force }
if (Test-Path -LiteralPath $art) { Remove-Item -LiteralPath $art -Recurse -Force }

"recipe $recipe SHA-256 $((Get-FileHash $recipe).Hash)" | Set-Content $log
"source $Source HEAD $(& git -C $Source rev-parse HEAD) ($(& git -C $Source rev-parse --abbrev-ref HEAD)), status lines $(@(& git -C $Source status --porcelain).Count)" | Add-Content $log

$a = @('-NoProfile', '-File', $recipe, '-Config', 'lavapipe', '-Source', $Source, '-Build', $build, '-Temp', $temp,
    '-Llvm', $Llvm, '-Arch', $Arch)
if ($Jobs -gt 0) { $a += @('-Jobs', $Jobs) }
if ($Python) { $a += @('-Python', $Python) }
if ($Ninja) { $a += @('-Ninja', $Ninja) }
if ($VsInstall) { $a += @('-VsInstall', $VsInstall) }
if ($Incremental -and (Test-Path -LiteralPath (Join-Path $build 'meson-private\coredata.dat'))) { $a += '-Reconfigure' }
& pwsh @a *>> $log
$code = $LASTEXITCODE
"build exit=$code" | Add-Content $log
if ($code -ne 0) { throw "build-mesa.ps1 -Config lavapipe failed ($code), see $log" }

$outDir = Join-Path $build 'src\gallium\targets\lavapipe'
$dll = Join-Path $outDir 'vulkan_lvp.dll'
$pdb = Join-Path $outDir 'vulkan_lvp.pdb'

$saved = Save-ProcessEnvironment
try {
    Import-VsDevEnvironment $VsInstall $temp $Arch | Out-Null
    $dumpbin = (Get-Command dumpbin.exe).Source
    $hdr = & $dumpbin /nologo /headers $dll | Select-String 'machine \('
    "header: $hdr" | Add-Content $log
    $want = if ($Arch -eq 'x86') { '14C machine \(x86\)' } else { '8664 machine \(x64\)' }
    if ("$hdr" -notmatch $want) { throw "machine gate FAILED: $dll is not $Arch ($hdr)" }
    'machine gate passed' | Add-Content $log
    $deps = & $dumpbin /nologo /dependents $dll
    $deps | Add-Content $log
    $crt = @($deps | Where-Object { $_ -match '^\s+(msvcp|vcruntime|concrt|ucrtbase|api-ms-win-crt-)\S*\.dll\s*$' })
    if ($crt.Count) { throw "CRT gate FAILED: $(($crt | ForEach-Object { $_.Trim() }) -join ', ')" }
    'CRT gate passed: no C/C++ runtime DLL import' | Add-Content $log
    $exports = & $dumpbin /nologo /exports $dll
    # x86 stdcall names carry no decoration in the .def export, so the plain name is the gate on both.
    if (-not ($exports | Select-String -SimpleMatch 'vk_icdGetInstanceProcAddr')) { throw 'export gate FAILED: no vk_icdGetInstanceProcAddr' }
    'export gate passed: vk_icdGetInstanceProcAddr' | Add-Content $log
} finally {
    Restore-ProcessEnvironment $saved
}

New-Item -ItemType Directory -Force $art | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $art 'vulkan_lvp.dll')
if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination (Join-Path $art 'vulkan_lvp.pdb') }
Copy-Item -LiteralPath (Join-Path $build 'recipe.json') -Destination (Join-Path $art 'recipe.json')
# The installed manifest names the DLL relative to the manifest's own directory: the package puts both side by side.
$icd = Get-Content -LiteralPath (Join-Path $outDir 'lvp_icd.json') -Raw | ConvertFrom-Json
$icd.ICD.library_path = '.\vulkan_lvp.dll'
[IO.File]::WriteAllText((Join-Path $art 'lvp_icd.json'), (($icd | ConvertTo-Json -Depth 5) -replace "`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
$lines = Get-ChildItem $art -File | Where-Object Name -ne 'SHA256SUMS' | Sort-Object Name |
    ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, $_.Name }
$lines | Set-Content -LiteralPath (Join-Path $art 'SHA256SUMS') -Encoding ascii
$lines | Add-Content $log
$lines
