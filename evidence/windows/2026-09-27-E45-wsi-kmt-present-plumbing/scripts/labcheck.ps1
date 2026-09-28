# Read-only look at the lab before the wsi-kmt-001 control: no writes.
$ErrorActionPreference = 'Continue'
"utc " + [DateTime]::UtcNow.ToString('o')
"registered_icd " + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash
"vkcube " + (Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vkcube.exe').Hash
"presentmon " + (Get-FileHash -LiteralPath 'C:\BC250\m12\presentmon\PresentMon-2.6.0-x64.exe').Hash
"icd_candidates_dir " + (Test-Path 'C:\BC250\m12\icd-candidates')
"wsi_kmt_dir " + (Test-Path 'C:\BC250\m12\wsi-kmt')
"cli " + (Test-Path 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe')
"tasks_running " + ((Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' } | ForEach-Object { $_.TaskName }) -join ', ')
"dwm " + ((Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1).Id)
"interactive_user " + (Get-CimInstance Win32_ComputerSystem).UserName
"vkcube_running " + ((Get-Process vkcube -ErrorAction SilentlyContinue | Measure-Object).Count)
& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' log summary 2>&1 | Select-Object -First 12
