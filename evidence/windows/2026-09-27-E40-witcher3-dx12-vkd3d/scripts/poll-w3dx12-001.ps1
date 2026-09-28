# Wait for witcher3-dx12-001, print its log, unregister the task, verify the game directory and baseline.
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\m12\witcher3-dx12'
$out = 'C:\BC250\m12\witcher3-dx12-001'
$task = 'BC250-M12-witcher3-dx12-001'
$deadline = (Get-Date).AddSeconds(240)
while (-not (Test-Path "$dir\done-w3dx12-001.json") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }
if (Test-Path "$dir\done-w3dx12-001.json") { Get-Content "$dir\done-w3dx12-001.json" } else { 'NO_DONE_FILE' }
"--- run log"
if (Test-Path "$dir\run-w3dx12-001.log") { Get-Content "$dir\run-w3dx12-001.log" }
"--- vkd3d log (first 40)"
if (Test-Path "$out\vkd3d.log") { Get-Content "$out\vkd3d.log" -TotalCount 40 }
"--- dxgi log"
Get-ChildItem "$out\*_dxgi.log" -ErrorAction SilentlyContinue | ForEach-Object { Get-Content $_.FullName -TotalCount 40 }
$t = Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
if ($t) { "task_state=$($t.State)"; if ($t.State -ne 'Running') { Unregister-ScheduledTask -TaskName $task -Confirm:$false; 'task_unregistered' } }
"--- game dir leftovers"
$game = 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12'
foreach ($n in 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll') { "$n present=" + (Test-Path -LiteralPath "$game\$n") }
"witcher3_procs=" + ((Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
"icd_registered=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
Get-ChildItem "$out\screen-*.png" -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name) $($_.Length)" }
