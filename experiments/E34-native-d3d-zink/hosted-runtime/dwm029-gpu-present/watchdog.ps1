$ErrorActionPreference='Stop'
Start-Sleep -Seconds 300
try {& C:\BC250\m13\dwm-hosted029\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted029\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted029\watchdog.log;exit 1}
& logman stop BC250G0Dwm029 -ets *> C:\BC250\m13\dwm-hosted029\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition029 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
