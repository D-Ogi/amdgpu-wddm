# Wait for the probe task, print its log, unregister the task, report the registered ICD hash.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\fl-probe001'
$task = 'BC250-E37-FL005'
$deadline = (Get-Date).AddSeconds(420)
while (-not (Test-Path "$dir\done-fl005.json") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }
if (Test-Path "$dir\done-fl005.json") { Get-Content "$dir\done-fl005.json" } else { 'NO_DONE_FILE' }
if (Test-Path "$dir\run-fl005.log") { Get-Content "$dir\run-fl005.log" }
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if ($t) { "task_state=$($t.State)"; if ($t.State -ne 'Running') { Unregister-ScheduledTask -TaskName $task -Confirm:$false; 'task_unregistered' } }
"icd_registered=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"probe_procs=" + ((Get-Process fl-probe -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
