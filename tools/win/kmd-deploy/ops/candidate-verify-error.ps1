# Read-only: why the candidate Verify of a kmd-deploy attempt failed (error text, result receipt, last health wait).
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}-deploy[0-9]{3}$')][string]$Attempt)
$d = "C:\BC250\m15\$Attempt"
foreach ($f in @('candidate-verify.err', 'candidate-verify-helper.json', 'candidate-result.json', 'worker.err', 'watch-error.txt', 'retain-rejected.txt', 'candidate-verify-cpu.json', 'candidate-verify-baseline-wait-1.json')) {
  $p = Join-Path $d $f
  if (Test-Path -LiteralPath $p) { "--- $f"; (Get-Content -LiteralPath $p -Raw) -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [Math]::Min(1200, $_.Length)) } }
}
"--- last candidate-verify-* files"
Get-ChildItem -LiteralPath $d -File | Where-Object { $_.Name -like 'candidate-verify-*' } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 5 | ForEach-Object { "=== $($_.Name) $($_.LastWriteTimeUtc.ToString('HH:mm:ss'))"; (Get-Content -LiteralPath $_.FullName -Raw) -replace '\s+', ' ' | ForEach-Object { $_.Substring(0, [Math]::Min(500, $_.Length)) } }
