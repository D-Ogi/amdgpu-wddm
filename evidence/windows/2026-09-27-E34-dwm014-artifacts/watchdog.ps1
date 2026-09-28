$ErrorActionPreference='Stop'
Start-Sleep -Seconds 240
try {& C:\BC250\m13\dwm-hosted014\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted014\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted014\watchdog.log;exit 1}
& logman stop BC250G0Dwm014 -ets *> C:\BC250\m13\dwm-hosted014\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition014 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
