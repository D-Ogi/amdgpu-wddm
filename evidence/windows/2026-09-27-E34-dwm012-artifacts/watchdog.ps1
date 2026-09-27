$ErrorActionPreference='Stop'
Start-Sleep -Seconds 240
try {& C:\BC250\m13\dwm-hosted012\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted012\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted012\watchdog.log;exit 1}
& logman stop BC250G0Dwm012 -ets *> C:\BC250\m13\dwm-hosted012\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition012 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
