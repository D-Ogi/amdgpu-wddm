& 'C:\BC250\tmp\heartbeat-start.ps1' 60
Remove-Item -LiteralPath 'C:\BC250\tmp\running.txt' -ErrorAction SilentlyContinue
& 'C:\BC250\tmp\postflight-probe-run.ps1' | Out-Null
"running seen by probe: $(Get-Content -LiteralPath 'C:\BC250\tmp\running.txt' -ErrorAction SilentlyContinue)"
Get-ScheduledTask | Where-Object { $_.State -eq 'Running' } | ForEach-Object { "running: $($_.TaskPath)$($_.TaskName)" } | Select-Object -First 30
& 'C:\BC250\tmp\heartbeat-stop.ps1'
