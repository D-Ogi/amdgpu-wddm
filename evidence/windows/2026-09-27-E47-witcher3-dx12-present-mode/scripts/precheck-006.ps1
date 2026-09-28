# Read-only precheck for runs 006/007: pushed files, candidate hash, registered ICD, tasks, game, temperature.
$ErrorActionPreference = 'Continue'
"candidate=" + (Get-FileHash -LiteralPath 'C:\BC250\m12\icd-candidates\vulkan_radeon.CF3948D6.dll').Hash
"registered=" + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash.Substring(0, 8)
foreach ($n in 'run-w3dx12-006.ps1', 'launch-w3dx12-006.ps1', 'worker-w3dx12-006.ps1', 'stop-006.ps1', 'status-006.ps1', 'run-w3dx12-007.ps1', 'launch-w3dx12-007.ps1', 'worker-w3dx12-007.ps1', 'stop-007.ps1', 'status-007.ps1', 'input-launch-dx12-003.ps1', 'input-dx12-003.ps1') {
  $p = "C:\BC250\m12\witcher3-dx12\$n"
  $ok = Test-Path -LiteralPath $p
  $parse = ''
  if ($ok) { $errs = $null; [void][System.Management.Automation.Language.Parser]::ParseFile($p, [ref]$null, [ref]$errs); $parse = if ($errs.Count) { "PARSE_ERRORS=$($errs.Count)" } else { 'parses' } }
  "$n present=$ok $parse"
}
"out006_exists=" + (Test-Path 'C:\BC250\m12\witcher3-dx12-006')
"out007_exists=" + (Test-Path 'C:\BC250\m12\witcher3-dx12-007')
"witcher3_procs=" + ((Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
"bc250_tasks=" + ((Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' } | ForEach-Object { "$($_.TaskName):$($_.State)" }) -join ' ')
"etw_sessions=" + ((logman query -ets 2>$null | Select-String 'BC250|PresentMon' | ForEach-Object { $_.Line.Trim() }) -join ' | ')
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String; if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "tctl=$($Matches[1])" }
"dwm=" + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' health read 2>&1 | Select-Object -First 6
