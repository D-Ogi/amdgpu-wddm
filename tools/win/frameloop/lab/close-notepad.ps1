# Closes only the Notepad processes the broken frameloop .cmd wrapper opened (2026-10-03 00:55-00:57 lab local).
$from = Get-Date '2026-10-03 00:55:00'; $to = Get-Date '2026-10-03 00:57:30'
$p = @(Get-Process -Name Notepad -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $from -and $_.StartTime -le $to })
$p | ForEach-Object { 'closing ' + $_.Id + ' started ' + $_.StartTime.ToString('HH:mm:ss') }
$p | Stop-Process -Force
Start-Sleep -Seconds 1
'left: ' + @(Get-Process -Name Notepad -ErrorAction SilentlyContinue).Count
