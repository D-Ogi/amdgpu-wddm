# Lists cases.tsv rows of a batch matching a pattern, with durations (us), plus each attempt's start/end and timing.
#   target.py ps case-times.ps1 -RunId icd85077e29 -Batch 30 -Pattern 512_256_6
param([string]$RunId, [int]$Batch, [string]$Pattern = '.', [string]$Results = 'C:\BC250\cts\results')
$dir = Join-Path (Join-Path $Results $RunId) ('batch-{0:D4}' -f $Batch)
Get-Content -LiteralPath (Join-Path $dir 'cases.tsv') | Where-Object { $_ -match $Pattern } | ForEach-Object {
    $f = $_ -split "`t"; "{0,12} {1,-8} a{2} {3}" -f $f[3], $f[1], $f[2], ($f[0] -replace '^dEQP-VK\.sparse_resources\.', '')
}
foreach ($j in Get-ChildItem -LiteralPath $dir -Filter 'attempt-*.json') { "--- $($j.Name)"; Get-Content -LiteralPath $j.FullName -Raw }
