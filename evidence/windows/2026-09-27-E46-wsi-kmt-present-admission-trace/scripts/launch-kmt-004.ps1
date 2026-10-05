# Launch wsi-kmt-004 (candidate ICD 0CD4A98D through VK_DRIVER_FILES, vkcube KMT present and CPU control) in the interactive session.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\wsi-kmt'
$task = 'BC250-M12-wsi-kmt-004'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if (Test-Path "$dir\done-kmt-004.json") { throw 'Existing run' }
if (Test-Path 'C:\BC250\m12\wsi-kmt-004') { throw 'Existing output' }
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { throw 'Existing task' }
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
if ($running) { throw ('A BC250 test task is running: ' + (($running | ForEach-Object { $_.TaskName }) -join ', ')) }
if (Get-Process vkcube -ErrorAction SilentlyContinue) { throw 'vkcube running' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D') { throw 'Registered ICD is not 93B1D1FD' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m12\icd-candidates\vulkan_radeon.0CD4A98D.dll').Hash -ne '0CD4A98DD80AE1248CFA1E6D4C1EB650DCF217FA9A840C002EDC4BB7EB0ADB47') { throw 'Candidate hash' }
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) { throw 'No interactive user' }
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker-kmt-004.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 8)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | ConvertTo-Json
