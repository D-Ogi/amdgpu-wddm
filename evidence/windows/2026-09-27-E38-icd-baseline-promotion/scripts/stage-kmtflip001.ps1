# Read-only precheck plus staging of the kmtflip001 output directory (no baseline change).
$ErrorActionPreference = 'Stop'
$out = 'C:\BC250\m13\kmt-flip001'
"time_utc=" + (Get-Date).ToUniversalTime().ToString('o')
try { "stop_flag=" + ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) } catch { "stop_flag=unreachable" }
"--- scheduled tasks BC250-*"
Get-ScheduledTask -TaskPath '\' -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like 'BC250*' } | ForEach-Object { "$($_.TaskName) $($_.State)" }
"--- processes of interest"
Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'runtime-|control|probe|bc250d3d|fl-probe' } |
  Select-Object Id, ProcessName | ForEach-Object { "$($_.Id) $($_.ProcessName)" }
"--- files"
foreach ($f in 'C:\BC250\m10\wsi-final\vulkan_radeon.dll', 'C:\BC250\m11\resource-close\bc250d3d.dll', 'C:\BC250\m11\resource-close\bc250d3d.m13-original.dll',
               'C:\BC250\m13\runtime-probe001\router.dll', 'C:\BC250\m13\runtime-probe001\bc250d3d_zink.dll', 'C:\BC250\m13\runtime-probe001\baseline.dll',
               'C:\BC250\m13\fork-consolidated001\bc250d3d_zink.dll', 'C:\BC250\m13\fork-consolidated001\vulkan_radeon.dll',
               'C:\BC250\m12\fl-probe001\candidate-kmt-enum\vulkan_radeon.dll', 'C:\BC250\m13\hosted-runtime016\runtime-flip-control.exe') {
  if (Test-Path -LiteralPath $f) { "$((Get-FileHash -LiteralPath $f).Hash.Substring(0,8)) $f" } else { "missing  $f" }
}
"--- dwm"
Get-Process dwm | ForEach-Object { "dwm pid=$($_.Id) start=$($_.StartTime)" }
if (Test-Path $out) { "out_exists=" + ((Get-ChildItem $out | Measure-Object).Count) } else { New-Item -ItemType Directory -Path $out | Out-Null; 'out_created' }
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "temperature=$($Matches[1])" }
