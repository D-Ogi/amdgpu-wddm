$ErrorActionPreference='Stop'
Start-Sleep -Seconds 300

try {try {& C:\BC250\m13\dwm-hosted038\stop-handshake.ps1} finally {& C:\BC250\m13\dwm-hosted038\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted038\watchdog.log}} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted038\watchdog.log;exit 1}
& logman stop BC250G0Dwm038 -ets *> C:\BC250\m13\dwm-hosted038\etw-watchdog-stop.log
Get-ScheduledTask -TaskName BC250-G0-Composition038 -ErrorAction SilentlyContinue | Stop-ScheduledTask -ErrorAction SilentlyContinue
