# Actual catalog membership; no installation, registry mutation, or trust-store import.
param([Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\firmware-package.ps1')
Test-DriverCatalogMembers $Directory (@('bc250kmd.inf', 'bc250kmd.sys', 'amdgpu-state.bin') + @(Get-DriverStoreFirmwareNames))
'Driver Store catalog: INF, SYS, state and all eight firmware members PASS'
