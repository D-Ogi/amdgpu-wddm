param([int]$Seconds = 150)
$dir = 'C:\BC250\tools\heartbeat'
$null = New-Item -ItemType Directory -Force -Path $dir
Copy-Item -LiteralPath 'C:\BC250\tmp\heartbeat.ps1' -Destination "$dir\heartbeat.ps1" -Force
$name = 'Lab-Present-Heartbeat'
Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$dir\heartbeat.ps1`" -Seconds $Seconds"
$p = New-ScheduledTaskPrincipal -UserId 'bc250' -LogonType Interactive
$s = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds + 60)) -AllowStartIfOnBatteries
$null = Register-ScheduledTask -TaskName $name -Action $a -Principal $p -Settings $s
Start-ScheduledTask -TaskName $name
Start-Sleep -Seconds 3
"task state $((Get-ScheduledTask -TaskName $name).State)"
