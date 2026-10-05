# Launch cpucopy-001 (two ICDs swapped in place in turn, vkcube CPU present with the timing log) in the interactive session.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\witcher3-dx12'
$task = 'BC250-M12-cpucopy-001'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if (Test-Path "$dir\done-cpucopy-001.json") { throw 'Existing run' }
if (Test-Path 'C:\BC250\m12\cpucopy-001') { throw 'Existing output' }
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { throw 'Existing task' }
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
if ($running) { throw ('A BC250 test task is running: ' + (($running | ForEach-Object { $_.TaskName }) -join ', ')) }
if (Get-Process vkcube -ErrorAction SilentlyContinue) { throw 'vkcube running' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D') { throw 'Registered ICD is not 93B1D1FD' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m12\icd-candidates\vulkan_radeon.50E99A84.dll').Hash -ne '50E99A84C1FEA6295E6D54AB3E516498462E84FB3651A24A52FE5F2E942C151F') { throw 'Candidate hash' }
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) { throw 'No interactive user' }
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker-cpucopy-001.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 8)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | ConvertTo-Json
