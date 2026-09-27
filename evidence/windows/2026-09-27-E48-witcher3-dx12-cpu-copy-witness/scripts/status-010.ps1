# Live status of run 010: last log lines, game process, input log.
$dir = 'C:\BC250\m12\witcher3-dx12'
if (Test-Path "$dir\run-w3dx12-010.log") { Get-Content "$dir\run-w3dx12-010.log" | Where-Object { $_ -notmatch '^t=' } | Select-Object -Last 12; Get-Content "$dir\run-w3dx12-010.log" | Where-Object { $_ -match '^t=' } | Select-Object -Last 1 }
Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { "game pid=$($_.Id) responding=$($_.Responding) title='$($_.MainWindowTitle)' ws_mb=$([int]($_.WorkingSet64/1MB))" }
if (Test-Path "$dir\input-010.log") { Get-Content "$dir\input-010.log" -Tail 3 }
