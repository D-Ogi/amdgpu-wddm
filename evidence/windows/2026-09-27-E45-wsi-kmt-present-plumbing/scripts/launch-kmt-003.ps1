# Launch wsi-kmt-003 (candidate ICD 11BA2143 through VK_DRIVER_FILES, vkcube KMT present and CPU control) in the interactive session.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\wsi-kmt'
$task = 'BC250-M12-wsi-kmt-003'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if (Test-Path "$dir\done-kmt-003.json") { throw 'Existing run' }
if (Test-Path 'C:\BC250\m12\wsi-kmt-003') { throw 'Existing output' }
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { throw 'Existing task' }
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
if ($running) { throw ('A BC250 test task is running: ' + (($running | ForEach-Object { $_.TaskName }) -join ', ')) }
if (Get-Process vkcube -ErrorAction SilentlyContinue) { throw 'vkcube running' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D') { throw 'Registered ICD is not 93B1D1FD' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m12\icd-candidates\vulkan_radeon.11BA2143.dll').Hash -ne '11BA214350632A0DA665521D7132318DA62043C5D7EAFE7BFF7EF1BDB48FEFF2') { throw 'Candidate hash' }
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) { throw 'No interactive user' }
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker-kmt-003.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 8)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | ConvertTo-Json
