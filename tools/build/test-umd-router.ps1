#Requires -Version 7.0
# Host gate of the application router (driver/umd/router), without a device and without touching machine state.
# Builds a scratch layout from the output of build-umd-router.ps1 (router, UMD doubles, harness) and three real
# binaries named by parameter: the hosted UMD bc250d3d_zink.dll, the CPU UMD bc250d3d.dll and a package holding the
# DXVK shell amdgpu_wddm_d3d11.dll with its amdgpu_wddm_d3d11.config. Then runs test-router.exe, each scenario in its
# own process on a private application hive (RegLoadAppKey, RegOverridePredefKey). Every scenario of the desktop-only
# router 5BBEB783's gate runs unchanged next to the AppRouter ones. The gate of 674AD261 (2026-10-01) used the
# hosted UMD E6B944CF, the CPU UMD 4176D1DF and the shell E748418C with its config A9B498ED: 68 scenarios, 0 failed.
#
# M15.14 increment 1 adds eleven scenarios, which 'run' takes with the rest: three pure suites of the D3D11_1 front
# (front-tables walks both filled tables slot by slot and names every copied and own entry, front-rule drives the
# DirectFlip rule, front-record drives the E26R decode and the resource map) and eight hive scenarios for the states
# of DirectFlipFront (absent, 0, a wrong type, on, the D3D10 entry point, a device created at the D3D10.0 interface,
# the CPU route, and the front over the real hosted UMD). 80 scenarios, 0 failed on 2026-10-06.
#
# M15.14 increment 2 adds front-answer: the front over the double, with surfaces it recorded through its own
# CreateResource and OpenResource hooks and the scan-out caps trailer in the adapter query. The pair the lab passes
# is answered TRUE, and seven negative controls each turn it FALSE under the clause they name (no trailer, the flag
# clear, the source mode moved, no SCANOUT bit, a v2 record, two pitches, a destroyed client). front-on keeps the
# FALSE of a start without the trailer. 82 scenarios, 0 failed on 2026-10-07 with the tester.20 hosted UMD, CPU UMD
# and D3D11 package.
#
# The per-application RenderOnCpu setting (docs/design/per-app-graphics-settings.md) adds six hive scenarios: the
# application key, the global key, an application 0 over a global 1, the environment over the registry, ignored
# values (out of range, a wrong type) and the Deny list over an application 0. 88 scenarios, 0 failed on 2026-10-08
# with the same tester.20 binaries.
#
#   pwsh tools\build\test-umd-router.ps1 -HostedUmd <bc250d3d_zink.dll> -CpuUmd <bc250d3d.dll> -AppPackage <dir>
#        [-Build <build-umd-router.ps1 output>] [-OutputDir <dir>]
param(
    [Parameter(Mandatory)][string]$HostedUmd,
    [Parameter(Mandatory)][string]$CpuUmd,
    [Parameter(Mandatory)][string]$AppPackage,
    [string]$Build,
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
if (-not $Build) { $Build = Join-Path $root 'scratch\build\umd-router' }
if (-not $OutputDir) { $OutputDir = Join-Path $root 'scratch\build\umd-router-host-gates' }
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$gate = Join-Path ([IO.Path]::GetFullPath($OutputDir)) $stamp
$layout = Join-Path $gate 'layout'
$out = Join-Path $gate 'results'
foreach ($d in @('router','router-wow','cpu','cpu-real','fail','umd','umd-noicd','umd-diag','umd-diag\logs','umd-router','icd-alt','dwm','hives',
                 'routelogs','diaglogs','diaglogs-env','app','app-noexport','app-real','app-real-noconfig','logonui','applogs')) {
    [void](New-Item -ItemType Directory -Force -Path (Join-Path $layout $d))
}
[void](New-Item -ItemType Directory -Force -Path $out)
function Put([string]$From, [string]$To) { Copy-Item -LiteralPath $From -Destination (Join-Path $layout $To) }
# Placeholder ICD: OpenAdapter only checks that the file exists; the ICD is loaded at CreateDevice.
function Placeholder([string]$To) { Set-Content -LiteralPath (Join-Path $layout $To) -Value 'placeholder, never loaded by OpenAdapter' -NoNewline }
$shell = Join-Path $AppPackage 'amdgpu_wddm_d3d11.dll'
$shellConfig = Join-Path $AppPackage 'amdgpu_wddm_d3d11.config'
Put "$Build\bc250d3d_router.dll" 'router\bc250d3d_router.dll'
Put "$Build\fake-hosted.dll" 'router\bc250d3d_zink.dll'
Put "$Build\fake-cpu.dll" 'cpu\bc250d3d.dll'
# The 32-bit router's default CPU UMD is bc250d3d.dll next to it (route-wow-default-cpu-beside-router).
Put "$Build\bc250d3d_router.dll" 'router-wow\bc250d3d_router.dll'
Put "$Build\fake-cpu.dll" 'router-wow\bc250d3d.dll'
Put "$Build\fake-fail.dll" 'fail\fake-fail.dll'
Put "$Build\fake-app.dll" 'app\amdgpu_wddm_d3d11.dll'
Put "$Build\fake-nooa102.dll" 'app-noexport\amdgpu_wddm_d3d11.dll'
Put $CpuUmd 'cpu-real\bc250d3d.dll'
foreach ($d in @('umd','umd-noicd','umd-diag','umd-router')) { Put $HostedUmd "$d\bc250d3d_zink.dll" }
foreach ($d in @('umd','umd-diag','umd-router')) { Placeholder "$d\amdgpu_wddm_radv.dll" }
Placeholder 'icd-alt\alt-icd.dll'
Put "$Build\bc250d3d_router.dll" 'umd-router\bc250d3d_router.dll'
Put "$Build\test-router.exe" 'dwm\dwm.exe'
Put "$Build\test-router.exe" 'logonui\logonui.exe'
# The DXVK shell reads its config beside itself at OpenAdapter; engine and ICD are loaded (and hash-checked) only
# at CreateDevice, so the host layout holds the shell and the config alone.
Put $shell 'app-real\amdgpu_wddm_d3d11.dll'
Put $shellConfig 'app-real\amdgpu_wddm_d3d11.config'
Put $shell 'app-real-noconfig\amdgpu_wddm_d3d11.dll'

$inputs = foreach ($f in @("$Build\bc250d3d_router.dll", "$Build\test-router.exe", $HostedUmd, $CpuUmd, $shell, $shellConfig)) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $f).Hash, $f
}
$inputs | Set-Content -LiteralPath (Join-Path $gate 'inputs.sha256')
& "$Build\test-router.exe" run $layout $out | Tee-Object -FilePath (Join-Path $gate 'run.txt')
$code = $LASTEXITCODE
"exit=$code" | Add-Content -LiteralPath (Join-Path $gate 'run.txt')
Write-Output "host gate directory: $gate"
exit $code
