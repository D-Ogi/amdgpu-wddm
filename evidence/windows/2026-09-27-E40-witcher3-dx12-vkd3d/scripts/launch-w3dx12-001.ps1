# Launch witcher3-dx12-001 in the interactive session (the game needs a desktop window).
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m12\witcher3-dx12'
$task = 'BC250-M12-witcher3-dx12-001'
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if (Test-Path "$dir\done-w3dx12-001.json") { throw 'Existing run' }
if (Test-Path 'C:\BC250\m12\witcher3-dx12-001') { throw 'Existing output' }
if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { throw 'Existing task' }
$running = Get-ScheduledTask -TaskPath '\' | Where-Object { $_.TaskName -like 'BC250*' -and $_.State -eq 'Running' -and $_.TaskName -notin 'BC250 monitor overlay', 'BC250 net watchdog' }
if ($running) { throw ('A BC250 test task is running: ' + (($running | ForEach-Object { $_.TaskName }) -join ', ')) }
if (Get-Process witcher3 -ErrorAction SilentlyContinue) { throw 'Game running' }
if ((Get-FileHash -LiteralPath 'C:\BC250\m10\wsi-final\vulkan_radeon.dll').Hash -ne '93B1D1FDB3E922F74102630DC013EA4BCC258AD7BD52078FCB014187019D738D') { throw 'Registered ICD is not the promoted baseline' }
$game = 'C:\Program Files (x86)\Steam\steamapps\common\The Witcher 3\bin\x64_dx12'
foreach ($n in 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll') { if (Test-Path -LiteralPath "$game\$n") { "note: $n already present in game dir" } }
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) { throw 'No interactive user' }
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $dir\worker-w3dx12-001.ps1"
$principal = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 6)
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $task
Get-ScheduledTask -TaskName $task | Select-Object TaskName, State | ConvertTo-Json
