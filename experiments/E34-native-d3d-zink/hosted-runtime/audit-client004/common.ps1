$ErrorActionPreference='Stop'
$d=$PSScriptRoot
if($d -ne 'C:\BC250\m13\audit-client004'){throw 'Runner must use its exact staged directory'}
. "$d\durable.ps1"
$cfg=Get-Content -LiteralPath "$d\manifest.json" -Raw | ConvertFrom-Json
$workerTask='BC250-G0-AuditClient004'
$watchdogTask='BC250-G0-AuditWatch004'
$items=@(
 @{path='C:\BC250\m11\resource-close\bc250d3d.dll';backup="$d\baseline-umd.dll";original="$d\original-umd.dll";baseline='8279AC7F6342CD0A31CB96531194CEF4A224A0D010BCEE4B549D9606D5E405EA';candidate=$cfg.'router.dll';source="$d\router.dll"},
 @{path='C:\BC250\m10\wsi-final\vulkan_radeon.dll';backup="$d\baseline-icd.dll";original="$d\original-icd.dll";baseline='CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157';candidate='7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E';source='C:\BC250\m13\shared-import001\vulkan_radeon.dll'}
)
function Assert-Stage {
 foreach($entry in $cfg.PSObject.Properties){
  $path=Join-Path $d $entry.Name
  if((Get-FileHash -LiteralPath $path).Hash -ne $entry.Value){throw "Stage hash mismatch: $($entry.Name)"}
 }
}
function Get-Control {
 @(Get-Process -Name runtime-audit-control -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq "$d\runtime-audit-control.exe"})
}
function Assert-StopThermal {
 if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
 $temp=(& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 | Out-String)
 if($LASTEXITCODE -ne 0 -or $temp -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'Temperature unavailable'}
 if([double]$Matches[1] -ge 85){throw 'Temperature limit'}
}
