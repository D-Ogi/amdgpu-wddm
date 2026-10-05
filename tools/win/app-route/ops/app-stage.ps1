# LAB (elevated SSH): admits the pushed package and prepares its directories. Changes nothing outside <Root>.
#  - every SHA256SUMS.txt entry present and equal, app-route.json well formed;
#  - logs\ (route lines of every application process): Users and both application-package groups may write, with a
#    low mandatory label, so a low-integrity or AppContainer process (a browser's GPU process, a UWP shell app) can
#    append its line; the router skips the line silently where it cannot;
#  - gpu\ and clients\: read and execute for both application-package groups (S-1-15-2-1 ALL APPLICATION PACKAGES,
#    S-1-15-2-2 ALL RESTRICTED APPLICATION PACKAGES), so an AppContainer process can load the GPU UMD at all;
#  - runs\ and receipts\ for the other scripts.
# Reports the ACLs (SDDL) of these and of the registered router's and the CPU UMD's directories: an AppContainer
# process that cannot read those never reaches the router in the first place.
param([string]$Root='C:\BC250\m14\app-route-001')
$ErrorActionPreference='Stop'
. "$Root\ops\durable.ps1"
. "$Root\ops\approute-lib.ps1"
$stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$r=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');root=$Root}
function Invoke-Icacls([string[]]$Arguments){
 $out=& icacls.exe @Arguments 2>&1 | Out-String
 if($LASTEXITCODE -ne 0){throw "icacls $($Arguments -join ' '): $out"}
}
try {
 $m=Read-AppRouteManifest $Root
 $pkg=Test-AppRoutePackage $Root
 $r.package_files=$pkg.files
 if($pkg.mismatched.Count){throw "Package files changed or missing: $($pkg.mismatched -join ',')"}
 foreach($d in @('logs','runs','receipts')){$null=New-Item -ItemType Directory -Force -Path (Join-Path $Root $d)}
 Invoke-Icacls @("$Root\logs",'/grant','*S-1-5-32-545:(OI)(CI)M','*S-1-15-2-1:(OI)(CI)M','*S-1-15-2-2:(OI)(CI)M')
 Invoke-Icacls @("$Root\logs",'/setintegritylevel','(OI)(CI)low')
 foreach($d in @($m.gpu.dir,'clients')){
  Invoke-Icacls @((Join-Path $Root $d),'/grant','*S-1-5-32-545:(OI)(CI)RX','*S-1-15-2-1:(OI)(CI)RX','*S-1-15-2-2:(OI)(CI)RX')
 }
 $acl=[ordered]@{}
 foreach($p in @("$Root\logs",(Join-Path $Root $m.gpu.dir),(Split-Path -Parent $m.router.active_path),(Split-Path -Parent $m.cpu_umd_path))){
  $acl[$p]=if(Test-Path -LiteralPath $p){(Get-Acl -LiteralPath $p).Sddl}else{'missing'}
 }
 $r.acl=$acl
 $r.outcome='ok'
} catch {
 $r.outcome='failed';$r.error=[string]$_
}
$json=$r|ConvertTo-Json -Depth 6
if(Test-Path -LiteralPath "$Root\receipts"){Write-DurableText "$Root\receipts\stage-$stamp.json" $json}
$json
if($r.outcome -ne 'ok'){exit 1}
