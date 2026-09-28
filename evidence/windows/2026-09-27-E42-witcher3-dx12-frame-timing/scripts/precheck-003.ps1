# Pre-window check for run 003: file hashes on the lab, no test task, baseline ICD, PresentMon runs at all (help only).
$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\m12\witcher3-dx12'
foreach ($n in 'run-w3dx12-003.ps1', 'worker-w3dx12-003.ps1', 'stop-003.ps1', 'status-003.ps1', 'input-dx12-003.ps1') {
  if (Test-Path "$dir\$n") { "$n " + (Get-FileHash -LiteralPath "$dir\$n").Hash.Substring(0, 8) } else { "$n MISSING" }
}
$pm = 'C:\BC250\m12\presentmon\PresentMon-2.6.0-x64.exe'
"presentmon " + (Get-FileHash -LiteralPath $pm).Hash + " " + (Get-Item -LiteralPath $pm).Length
"presentmon_help_first_line: " + ((& $pm --help 2>&1 | Select-Object -First 1) -join ' ')
"icd " + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
"umd " + (Get-FileHash -LiteralPath 'C:\BC250\m13\hosted-runtime\bc250_d3d10umd.dll' -ErrorAction SilentlyContinue).Hash
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
"running_test_tasks=" + (($running | ForEach-Object { $_.TaskName }) -join ',')
"registered_bc250_tasks=" + ((Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' } | ForEach-Object { "$($_.TaskName):$($_.State)" }) -join ',')
"witcher3=" + ((Get-Process witcher3 -ErrorAction SilentlyContinue | Measure-Object).Count)
"dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
"stop_flag=" + (Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "tctl=$($Matches[1])" }
& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' clock read
