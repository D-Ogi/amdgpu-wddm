$ErrorActionPreference='Stop'
Start-Sleep -Seconds 300
try {& C:\BC250\m13\dwm-hosted031\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted031\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted031\watchdog.log;exit 1}
& logman stop BC250G0Dwm031 -ets *> C:\BC250\m13\dwm-hosted031\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition031 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
