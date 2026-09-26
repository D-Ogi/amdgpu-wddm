$ErrorActionPreference='Stop'
Start-Sleep -Seconds 60
try {& C:\BC250\m13\dwm-hosted010\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted010\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted010\watchdog.log;exit 1}

& logman stop BC250G0Dwm010 -ets *> C:\BC250\m13\dwm-hosted010\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition010 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
