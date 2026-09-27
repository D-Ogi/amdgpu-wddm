# Ask run 010 to end gracefully (the worker restores everything), then wait for its done file and print the tail.
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\m12\witcher3-dx12'
New-Item -ItemType File -Path "$dir\stop-010" -Force | Out-Null
$deadline = (Get-Date).AddSeconds(60)
while (-not (Test-Path "$dir\done-w3dx12-010.json") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
if (Test-Path "$dir\done-w3dx12-010.json") { Get-Content "$dir\done-w3dx12-010.json" } else { 'NO_DONE_FILE' }
if (Test-Path "$dir\run-w3dx12-010.log") { Get-Content "$dir\run-w3dx12-010.log" | Select-Object -Last 25 }
$t = Get-ScheduledTask -TaskName 'BC250-M12-witcher3-dx12-010' -ErrorAction SilentlyContinue
if ($t) { "task_state=$($t.State)"; if ($t.State -ne 'Running') { Unregister-ScheduledTask -TaskName 'BC250-M12-witcher3-dx12-010' -Confirm:$false; 'task_unregistered' } }
$game = 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12'
foreach ($n in 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll') { "$n present=" + (Test-Path -LiteralPath "$game\$n") }
"witcher3_procs=" + ((Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
"icd_registered=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
