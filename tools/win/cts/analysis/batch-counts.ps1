# Status counts and non-pass cases of one batch of one run (reads the runner's cases.tsv files under results\<run>).
param([Parameter(Mandatory)][string]$RunId, [Parameter(Mandatory)][int]$Index, [string]$Root = 'C:\BC250\cts')
$dir = Join-Path (Join-Path $Root 'results') $RunId
$b = 'batch-{0:D4}' -f $Index
Get-ChildItem -LiteralPath $dir -Recurse -Filter '*.tsv' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $b } | ForEach-Object {
    "== $($_.FullName.Substring($dir.Length))"
    $rows = Import-Csv -LiteralPath $_.FullName -Delimiter "`t"
    $rows | Group-Object { $_.status } | ForEach-Object { "{0} {1}" -f $_.Name, $_.Count }
    $rows | Where-Object { $_.status -notin @('Pass', 'NotSupported', 'QualityWarning', 'CompatibilityWarning') } | Select-Object -First 12 | ForEach-Object { "  $($_.status) $($_.case)" }
}
Get-ChildItem -LiteralPath $dir -Recurse -Filter "$b*.json" -ErrorAction SilentlyContinue | Select-Object -Last 3 | ForEach-Object { "== $($_.Name)"; (Get-Content -LiteralPath $_.FullName -Raw).Substring(0, [Math]::Min(600, $_.Length)) }
