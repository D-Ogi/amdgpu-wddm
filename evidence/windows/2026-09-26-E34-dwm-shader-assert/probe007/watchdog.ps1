$ErrorActionPreference='Stop'
Start-Sleep -Seconds 60
try {& C:\BC250\m13\dwm-hosted007\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted007\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted007\watchdog.log;exit 1}
