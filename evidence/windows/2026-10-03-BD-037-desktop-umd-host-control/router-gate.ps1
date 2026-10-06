#Requires -Version 7.0
# Host gate of the application-routing router 674AD261 with the BD-037 desktop UMDs: scratch\m14\dcomp-a8\router-gate.ps1
# with its output root here (host-gates\<UTC stamp>\) and the hosted and CPU UMD built in this directory. Every scenario
# of the deployed gate runs unchanged; no device, no machine state.
param(
    [string]$HostedUmd = 'P:\bc-250\scratch\bd037\artifacts\zink\bc250d3d_zink.dll',
    [string]$CpuUmd = 'P:\bc-250\scratch\bd037\artifacts\cpu\bc250d3d.dll',
    [string]$AppPackage = 'P:\bc-250\scratch\m14\dcomp-a8\stage\app-route-dcomp\gpu',
    [string]$Build = 'P:\BC-250\scratch\m15\app-route\build-router'
)
$ErrorActionPreference = 'Stop'
$source = 'P:\bc-250\scratch\m15\app-route\router\run-host-tests.ps1'
$text = Get-Content -LiteralPath $source -Raw
$from = "`$root = Join-Path 'P:\BC-250\scratch\m15\app-route\work\host-gates' `$stamp"
if (-not $text.Contains($from)) { throw "$source changed: its output root line is not the expected one" }
$copy = Join-Path $PSScriptRoot 'work\router-gate-run-host-tests.ps1'
New-Item -ItemType Directory -Force (Split-Path $copy) | Out-Null
Set-Content -LiteralPath $copy -Value $text.Replace($from, "`$root = Join-Path '$PSScriptRoot\host-gates' `$stamp") -NoNewline
& $copy -HostedUmd $HostedUmd -CpuUmd $CpuUmd -AppPackage $AppPackage -Build $Build
exit $LASTEXITCODE
