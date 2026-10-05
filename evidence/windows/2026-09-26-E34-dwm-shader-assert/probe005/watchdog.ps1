$ErrorActionPreference='Stop'
Start-Sleep -Seconds 30
try {& C:\BC250\m13\dwm-hosted005\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted005\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted005\watchdog.log;exit 1}
