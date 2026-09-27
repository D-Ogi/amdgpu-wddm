# Read-only preflight for the CF3948D6 ICD promotion (M569 procedure). No writes.
$ErrorActionPreference='Stop'
$r=[ordered]@{}
$r.utc=[DateTime]::UtcNow.ToString('o')
$r.boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
try { $r.stop=(Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop } catch { $r.stop="unavailable: $($_.Exception.Message)" }
$dir='C:\BC250\m10\wsi-final'
$r.registered=@{}
foreach($f in Get-ChildItem -LiteralPath $dir -Filter 'vulkan_radeon*.dll' | Where-Object {$_.Name -notmatch 'held'}){
 $r.registered[$f.Name]=@{sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;written=$f.LastWriteTimeUtc.ToString('o')}
}
$r.held_count=@(Get-ChildItem -LiteralPath $dir -Filter 'vulkan_radeon*held*').Count
$r.icd_manifest=(Get-FileHash -LiteralPath "$dir\radeon_icd.json").Hash
$r.khronos_drivers=@{}
$k=Get-Item 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' -ErrorAction SilentlyContinue
if($k){ foreach($n in $k.GetValueNames()){ $r.khronos_drivers[$n]=$k.GetValue($n) } }
$cand='C:\BC250\m12\icd-candidates'
$r.candidates=@{}
if(Test-Path $cand){ foreach($f in Get-ChildItem -LiteralPath $cand -Filter 'vulkan_radeon*.dll'){ $r.candidates[$f.Name]=(Get-FileHash -LiteralPath $f.FullName).Hash } }
$r.tasks=@(Get-ScheduledTask | Where-Object {$_.TaskName -like 'BC250*'} | ForEach-Object {@{name=$_.TaskName;state=[string]$_.State}})
$r.processes=@(Get-Process witcher3,deqp-vk,vkcube,cross-process-control,cross-process-duplicate-control,gfx-blt-control,composition-control -ErrorAction SilentlyContinue | ForEach-Object {$_.ProcessName})
$r.dwm=@(Get-Process dwm | ForEach-Object {$_.Id})
$t=(& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1 2>&1 | Out-String)
$r.tctl=if($t -match 'Tctl\s+([0-9.]+)\s+C'){[double]$Matches[1]}else{$t.Trim()}
$h=(& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read 2>&1 | Out-String)
$r.health=$h.Trim()
$reg=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$r.gates=@{interop=$reg.EnableCddDwmInterop;gpu_present=$reg.EnableGpuPresentBlit;unconfirmed=$reg.UnconfirmedStarts}
$r | ConvertTo-Json -Depth 5
