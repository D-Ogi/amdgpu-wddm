# Read-only: the supervisor's receipts of a kmd-deploy attempt on the lab (why did it end recovery-required?).
param([Parameter(Mandatory)][ValidatePattern('^kmd[0-9]{3}(-(?!1-)[1-9][0-9]*)?-deploy[0-9]{3}$')][string]$Attempt)
$d = "C:\BC250\m15\$Attempt"
"--- files"
Get-ChildItem -LiteralPath $d -File -Recurse | Where-Object { $_.Name -match '\.(json|txt|log|err|out)$' -and $_.Length -gt 0 } | Sort-Object LastWriteTimeUtc | ForEach-Object { '{0} {1} {2}' -f $_.FullName.Substring($d.Length), $_.Length, $_.LastWriteTimeUtc.ToString('HH:mm:ss') }
foreach ($f in @('result.json', 'supervisor-result.json', 'transition-result.json')) {
  $p = Join-Path $d $f
  if (Test-Path -LiteralPath $p) { "--- $f"; Get-Content -LiteralPath $p -Raw }
}
"--- receipts mentioning verify/health/confirm/restore (last 40 lines each, newest 4 files)"
Get-ChildItem -LiteralPath $d -File -Recurse | Where-Object { $_.Name -match 'verify|health|confirm|restore|phase|log' -and $_.Name -match '\.(json|txt|log)$' -and $_.Length -gt 0 } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 4 | ForEach-Object {
  "=== $($_.Name) $($_.LastWriteTimeUtc.ToString('HH:mm:ss'))"
  Get-Content -LiteralPath $_.FullName -Tail 40 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
}
"--- registry"
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
(Get-Item $k).Property | Where-Object { $_ -match '^(Unconfirmed|LastStage|StageHistory|KeepStatus|Dpm|Cu)' } | ForEach-Object { "$_ = $((Get-ItemProperty $k).$_)" }
"--- driverstore"
# Keep the complete package record: field labels are localized, INF filenames are not.
# This is a report only; it never selects a package for removal.
function Select-DriverStoreRecords([string[]]$Lines) {
  $blocks = (($Lines -join "`n") -split '(?:\r?\n)[\t ]*(?:\r?\n)')
  @($blocks | Where-Object { $_ -match '(?i)(?<![\w.-])oem[0-9]+\.inf(?![\w.-])' } | Select-Object -Last 4)
}
$driverStore = @(& pnputil.exe /enum-drivers 2>&1)
if ($LASTEXITCODE -ne 0) { throw "Driver-store query failed (pnputil exit $LASTEXITCODE)" }
Select-DriverStoreRecords $driverStore | ForEach-Object { $_; '' }
