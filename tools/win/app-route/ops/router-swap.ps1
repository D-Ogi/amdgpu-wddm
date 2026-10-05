# LAB (elevated SSH): swaps the registered desktop router file in place for the application-routing router, or back.
# Same path, so the UserModeDriverName registration does not change and no adapter restart is needed: every process
# that opens a D3D10/D3D11 adapter afterwards loads the new file; running processes keep the router they mapped
# (DWM included, until its next restart). The old file is renamed aside (held), never deleted while mapped.
#   -Action install   baseline -> candidate (refused under owner STOP or while a transition task runs)
#   -Action rollback  candidate -> baseline from the package's baseline copy (never refused by STOP)
#   -Action status    read-only
#   -Action cleanup   removes held/prepared leftovers that no process maps any more
# Receipt: <Root>\receipts\swap-<action>-<UTC>.json, also on stdout.
param([Parameter(Mandatory)][ValidateSet('install','rollback','status','cleanup')][string]$Action,
      [string]$Root='C:\BC250\m14\app-route-001')
$ErrorActionPreference='Stop'
. "$Root\ops\durable.ps1"
. "$Root\ops\approute-lib.ps1"
$stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$r=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');action=$Action;root=$Root}
function Get-Mappers([string]$Path){
 # Processes that map a module from this exact path (renamed files keep their load-time path in the module list).
 @(Get-Process | ForEach-Object {
  $p=$_
  try{ if(@($p.Modules | Where-Object { $_.FileName -ieq $Path }).Count){ "$($p.ProcessName):$($p.Id)" } }catch{}
 })
}
try {
 $m=Read-AppRouteManifest $Root
 $active=[string]$m.router.active_path
 $r.active=$active
 $r.baseline=$m.router.baseline_sha256; $r.candidate=$m.router.sha256
 $gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
 if($gpu.Count -ne 1){throw 'Expected one BC-250 display device'}
 $driverKey=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
 $umd=@((Get-ItemProperty ('HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+$driverKey)).UserModeDriverName)
 $r.registration=$umd
 $registered=@($umd | Where-Object { $_ -ieq $active }).Count -gt 0
 $r.registered=$registered
 $r.before=Get-AppFileSha $active
 switch($Action){
  'status' {
   $dir=Split-Path -Parent $active; $leaf=Split-Path -Leaf $active
   $r.leftovers=@(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -like ($leaf+'.app-route-*') } | ForEach-Object { @{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash} })
   $r.state=if($r.before -eq $m.router.sha256){'candidate'}elseif($r.before -eq $m.router.baseline_sha256){'baseline'}else{'foreign'}
   $r.mappers_active_path=Get-Mappers $active
  }
  'cleanup' { $r.leftovers=Remove-AppRouterLeftovers $active }
  'install' {
   if(!$registered){throw 'The registration does not name the active router path; a swap would route nothing'}
   if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop){throw 'Owner STOP'}
   if(@(Get-ScheduledTask | Where-Object { $_.State -eq 'Running' -and $_.TaskName -in @('BC250-UMD-Watch','BC250-KMD-Watch') }).Count){throw 'A transition task is running'}
   $pkg=Test-AppRoutePackage $Root
   if($pkg.mismatched.Count){throw "Package files changed: $($pkg.mismatched -join ',')"}
   $r.result=Install-AppRouter $active (Join-Path $Root $m.router.file) $m.router.baseline_sha256 $m.router.sha256 $stamp
  }
  'rollback' {
   $r.result=Restore-AppRouter $active (Join-Path $Root $m.router.baseline_file) $m.router.baseline_sha256 $m.router.sha256 $stamp
  }
 }
 $r.after=Get-AppFileSha $active
 $r.outcome='ok'
} catch {
 $r.outcome='failed';$r.error=[string]$_
}
$json=$r|ConvertTo-Json -Depth 6
if($Action -ne 'status'){
 $null=New-Item -ItemType Directory -Force -Path "$Root\receipts"
 Write-DurableText "$Root\receipts\swap-$Action-$stamp.json" $json
}
$json
if($r.outcome -ne 'ok'){exit 1}
