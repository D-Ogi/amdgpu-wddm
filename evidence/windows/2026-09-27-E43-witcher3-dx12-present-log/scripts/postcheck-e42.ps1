# After runs 003/004: no PresentMon process, no BC250W3 trace session, no test task, baseline ICD, game dir clean.
$ErrorActionPreference = 'Continue'
"presentmon_procs=" + ((Get-Process PresentMon-2.6.0-x64 -ErrorAction SilentlyContinue | Measure-Object).Count)
$q = & logman query BC250W3 -ets 2>&1 | Out-String
"logman_bc250w3=" + $(if ($q -match 'Status:\s+Running') { 'RUNNING' } elseif ($q -match 'does not exist|not found|nie istnieje|0x80070002|Data Collector Set was not found') { 'absent' } else { 'unknown: ' + ($q.Trim() -split "`n")[0] })
"witcher3_procs=" + ((Get-Process witcher3 -ErrorAction SilentlyContinue | Measure-Object).Count)
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
"running_test_tasks=" + (($running | ForEach-Object { $_.TaskName }) -join ',')
"w3_tasks=" + ((Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250-M12-witcher3-dx12-*' } | ForEach-Object { "$($_.TaskName):$($_.State)" }) -join ',')
$game = 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12'
foreach ($n in 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll') { "$n present=" + (Test-Path -LiteralPath "$game\$n") }
"icd=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "tctl=$($Matches[1])" }
& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' health read
