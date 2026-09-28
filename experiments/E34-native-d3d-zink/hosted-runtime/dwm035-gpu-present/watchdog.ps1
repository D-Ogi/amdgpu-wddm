$ErrorActionPreference='Stop'
Start-Sleep -Seconds 300
Get-ScheduledTask -TaskName BC250-G0-WsiColour035 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
Get-Process wsi-colour-control -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq 'C:\BC250\m13\dwm-hosted035\wsi-colour-control.exe'} | Stop-Process -Force -ErrorAction SilentlyContinue

try {& C:\BC250\m13\dwm-hosted035\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted035\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted035\watchdog.log;exit 1}
& logman stop BC250G0Dwm035 -ets *> C:\BC250\m13\dwm-hosted035\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition035 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
