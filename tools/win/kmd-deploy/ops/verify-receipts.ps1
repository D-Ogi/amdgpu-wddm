# Read-only: the verify-phase receipts of a kmd-deploy attempt (health witness lines) and the current health.
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}(-(?!1-)[1-9][0-9]*)?-deploy[0-9]{3}$')][string]$Attempt)
$d = "C:\BC250\m15\$Attempt"
foreach ($f in @('candidate-verify-health-before.txt', 'candidate-verify-health-wait-1.json', 'candidate-verify-health-wait-43.json', 'candidate-verify-readiness.json', 'watch-result.json', 'restore-verify-health-before.txt', 'restore-verify-readiness.json')) {
  $p = Join-Path $d $f
  if (Test-Path -LiteralPath $p) { "--- $f"; (Get-Content -LiteralPath $p -Raw) -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [Math]::Min(900, $_.Length)) } }
}
"--- newest verify files"
Get-ChildItem -LiteralPath $d -File | Where-Object { $_.Name -match 'verify' } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 6 | ForEach-Object { "=== $($_.Name) $($_.LastWriteTimeUtc.ToString('HH:mm:ss'))"; (Get-Content -LiteralPath $_.FullName -Raw) -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [Math]::Min(700, $_.Length)) } }
"--- health now"
& 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe' health read 2>&1 | Select-Object -First 1
"--- heartbeat task"
Get-ScheduledTask -TaskName 'Lab-Present-Heartbeat' -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1}' -f $_.TaskName, $_.State }
"--- session"
query user 2>&1 | Select-Object -First 4
