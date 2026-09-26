# Wait for the fork001 flip control to finish (done file), then print its receipt, log tail and restored hashes.
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\m13\fork-consolidated001'
$deadline = (Get-Date).AddSeconds(120)
while (-not (Test-Path "$dir\done-fork001.json") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
"task_state=" + ((Get-ScheduledTask -TaskName 'BC250-E36-Fork001' -ErrorAction SilentlyContinue).State)
if (Test-Path "$dir\done-fork001.json") { Get-Content "$dir\done-fork001.json" } else { 'NO DONE FILE' }
"--- run log (first 12 and last 12 lines)"
if (Test-Path "$dir\run-fork001.log") { $l = Get-Content "$dir\run-fork001.log"; $l | Select-Object -First 12; '...'; $l | Select-Object -Last 12 }
"--- restored baselines"
foreach ($f in 'C:\BC250\m10\wsi-final\vulkan_radeon.dll', 'C:\BC250\m11\resource-close\bc250d3d.dll', 'C:\BC250\m13\runtime-probe001\bc250d3d_zink.dll') {
  "$((Get-FileHash -LiteralPath $f).Hash.Substring(0,8)) $f"
}
"backup_left=" + (Test-Path 'C:\BC250\m11\resource-close\bc250d3d.m13-original.dll')
"--- captures"
Get-ChildItem "$dir\screen-fork001-*.png" -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name) $($_.Length)" }
Get-Process dwm | ForEach-Object { "dwm pid=$($_.Id) start=$($_.StartTime)" }
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "temperature=$($Matches[1])" }
