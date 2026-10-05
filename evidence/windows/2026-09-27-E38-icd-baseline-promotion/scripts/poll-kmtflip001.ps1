# Wait for the kmtflip001 control (done file), print receipt, log, restored hashes, unregister the task.
$ErrorActionPreference = 'Continue'
$out = 'C:\BC250\m13\kmt-flip001'
$task = 'BC250-KMT-Flip001'
$deadline = (Get-Date).AddSeconds(150)
while (-not (Test-Path "$out\done-kmtflip001.json") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
if (Test-Path "$out\done-kmtflip001.json") { Get-Content "$out\done-kmtflip001.json" } else { 'NO DONE FILE' }
"--- run log"
if (Test-Path "$out\run-kmtflip001.log") { Get-Content "$out\run-kmtflip001.log" }
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if ($t) { "task_state=$($t.State)"; if ($t.State -ne 'Running') { Unregister-ScheduledTask -TaskName $task -Confirm:$false; 'task_unregistered' } }
"--- restored baselines"
foreach ($f in 'C:\BC250\m10\wsi-final\vulkan_radeon.dll', 'C:\BC250\m11\resource-close\bc250d3d.dll', 'C:\BC250\m13\runtime-probe001\bc250d3d_zink.dll') {
  "$((Get-FileHash -LiteralPath $f).Hash.Substring(0,8)) $f"
}
"backup_left=" + (Test-Path 'C:\BC250\m11\resource-close\bc250d3d.m13-original.dll')
"baseline_dll_left=" + (Test-Path 'C:\BC250\m13\runtime-probe001\baseline.dll')
"--- captures"
Get-ChildItem "$out\screen-kmtflip001-*.png" -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name) $($_.Length)" }
Get-Process dwm | ForEach-Object { "dwm pid=$($_.Id) start=$($_.StartTime)" }
"control_procs=" + ((Get-Process runtime-flip-control -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "temperature=$($Matches[1])" }
