$ErrorActionPreference='Stop'
Start-Sleep -Seconds 300
try {& C:\BC250\m13\dwm-hosted026\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted026\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted026\watchdog.log;exit 1}
& logman stop BC250G0Dwm026 -ets *> C:\BC250\m13\dwm-hosted026\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition026 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
