# LAB (SSH), read-only: where applications' D3D10/D3D11 goes right now.
#  - the registered UMD names and the active router file (baseline, candidate or foreign);
#  - the AppRouter key and DesktopRouter's DwmForceCpu;
#  - DWM's filtered modules with hashes;
#  - per UMD (application GPU UMD, CPU UMD, hosted UMD): the processes that map it, by name and PID;
#  - the last -Tail lines of the route logs in <Root>\logs (newest files first).
# -NoMappers skips the per-process module scan (the slow part, a few seconds).
param([string]$Root='C:\BC250\m14\app-route-001',[ValidateRange(0,400)][int]$Tail=40,[switch]$NoMappers)
$ErrorActionPreference='Stop'
. "$Root\ops\approute-lib.ps1"
$r=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');root=$Root}
try {
 $m=Read-AppRouteManifest $Root
 $gpu=@(Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' })
 if($gpu.Count -eq 1){
  $r.gpu_status=[string]$gpu[0].Status
  $driverKey=(Get-PnpDeviceProperty -InstanceId $gpu[0].InstanceId -KeyName DEVPKEY_Device_Driver).Data
  $r.registration=@((Get-ItemProperty ('HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+$driverKey)).UserModeDriverName)
 }
 $active=[string]$m.router.active_path
 $h=Get-AppFileSha $active
 $r.router=[ordered]@{path=$active;sha256=$h;state=$(if($h -eq $m.router.sha256){'candidate'}elseif($h -eq $m.router.baseline_sha256){'baseline'}else{'foreign'})}
 $k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey([string]$m.policy_key,$false)
 try{$r.app_router=if($k){Read-AppKeyValues $k}else{'absent'}}finally{if($k){$k.Dispose()}}
 $k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\amdgpu-wddm\DesktopRouter',$false)
 try{$r.dwm_force_cpu=if($k){$k.GetValue('DwmForceCpu')}else{'key absent'}}finally{if($k){$k.Dispose()}}
 $r.dwm=@(Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object {
  @{pid=$_.Id;start=$_.StartTime.ToUniversalTime().ToString('o');modules=@($_.Modules | Where-Object { $_.ModuleName -match 'bc250|vulkan_radeon|amdgpu_wddm' } |
    ForEach-Object { @{name=$_.ModuleName;path=$_.FileName;sha256=(Get-AppFileSha $_.FileName)} })}
 })
 if(!$NoMappers){
  $shell=Join-Path (Join-Path $Root $m.gpu.dir) $m.gpu.shell
  $want=[ordered]@{app_gpu_umd=$shell;cpu_umd=[string]$m.cpu_umd_path;router=$active}
  $found=[ordered]@{}; foreach($n in $want.Keys){$found[$n]=New-Object System.Collections.Generic.List[string]}
  foreach($p in @(Get-Process)){
   try{$files=@($p.Modules | ForEach-Object { $_.FileName })}catch{continue}
   foreach($n in $want.Keys){ if(@($files | Where-Object { $_ -ieq $want[$n] }).Count){ $found[$n].Add("$($p.ProcessName):$($p.Id)") } }
   $hosted=@($files | Where-Object { $_ -match '\\bc250d3d_zink\.dll$' })
   if($hosted.Count){ if(!$found.Contains('hosted_umd')){$found['hosted_umd']=New-Object System.Collections.Generic.List[string]}; $found['hosted_umd'].Add("$($p.ProcessName):$($p.Id)") }
  }
  $r.mappers=$found
 }
 $lines=New-Object System.Collections.Generic.List[string]
 if($Tail -and (Test-Path -LiteralPath "$Root\logs")){
  foreach($f in @(Get-ChildItem -LiteralPath "$Root\logs" -Filter 'route-*.log' -File | Sort-Object LastWriteTimeUtc -Descending)){
   foreach($l in @(Get-Content -LiteralPath $f.FullName -Tail $Tail)){ if($lines.Count -lt $Tail){$lines.Add($l)} }
   if($lines.Count -ge $Tail){break}
  }
  $r.route_log_files=@(Get-ChildItem -LiteralPath "$Root\logs" -Filter 'route-*.log' -File).Count
 }
 $r.route_lines=$lines
 $r.outcome='ok'
} catch {
 $r.outcome='failed';$r.error=[string]$_
}
$r|ConvertTo-Json -Depth 6
if($r.outcome -ne 'ok'){exit 1}
