$ErrorActionPreference = 'Stop'
Stop-ScheduledTask -TaskName 'BC250 monitor overlay'
Get-Process bc250mon -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3
if (Get-Process bc250mon -ErrorAction SilentlyContinue) { throw 'Overlay still running' }
Copy-Item -LiteralPath 'C:\BC250\mon\m13\bc250mon.exe' -Destination 'C:\BC250\mon\bc250mon.exe' -Force
Get-FileHash 'C:\BC250\mon\bc250mon.exe' | Select-Object Hash
Start-ScheduledTask -TaskName 'BC250 monitor overlay'
Start-Sleep -Seconds 12
$s = Invoke-RestMethod http://127.0.0.1:2250/state
$s.panels | Where-Object { $_.name -eq 'graphics' } | ConvertTo-Json -Depth 8
'BOOT=' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
