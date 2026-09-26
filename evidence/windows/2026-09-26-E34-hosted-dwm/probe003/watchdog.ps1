$ErrorActionPreference='Stop'
Start-Sleep -Seconds 30
try {& C:\BC250\m13\dwm-hosted003\restore.ps1 -Restart *> C:\BC250\m13\dwm-hosted003\watchdog.log} catch {$_ | Out-String | Add-Content C:\BC250\m13\dwm-hosted003\watchdog.log;exit 1}
