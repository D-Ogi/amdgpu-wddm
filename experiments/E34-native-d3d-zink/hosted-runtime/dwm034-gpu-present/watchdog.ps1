$ErrorActionPreference='Stop'
Start-Sleep -Seconds 300
Get-ScheduledTask -TaskName BC250-G0-WsiColour034 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
Get-Process wsi-colour-control -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq 'C:\BC250\m13\dwm-hosted034\wsi-colour-control.exe'} | Stop-Process -Force -ErrorAction SilentlyContinue

try {& C:\BC250\m13\dwm-hosted034\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted034\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted034\watchdog.log;exit 1}
& logman stop BC250G0Dwm034 -ets *> C:\BC250\m13\dwm-hosted034\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition034 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
