# Prints the record of one case from a batch's attempts (cases.tsv line, the qpa block tail, stdout/stderr tails).
#   target.py ps case-detail.ps1 -RunId icd85077e29 -Batch 30 -Case <name>
param([string]$RunId, [int]$Batch, [string]$Case, [string]$Results = 'C:\BC250\cts\results', [int]$Lines = 40)
$dir = Join-Path (Join-Path $Results $RunId) ('batch-{0:D4}' -f $Batch)
"dir $dir"
Get-ChildItem -LiteralPath $dir | ForEach-Object { "{0,10} {1:u} {2}" -f $_.Length, $_.LastWriteTimeUtc, $_.Name }
"--- cases.tsv"
Get-Content -LiteralPath (Join-Path $dir 'cases.tsv') | Select-String -SimpleMatch $Case | ForEach-Object { $_.Line }
foreach ($qpa in Get-ChildItem -LiteralPath $dir -Filter 'attempt-*.qpa') {
    $text = [IO.File]::ReadAllText($qpa.FullName)
    $i = $text.IndexOf("#beginTestCaseResult $Case")
    if ($i -lt 0) { continue }
    "--- $($qpa.Name) from offset $i"
    $block = $text.Substring($i, [Math]::Min(6000, $text.Length - $i))
    ($block -split "`n" | Where-Object { $_ -notmatch '^\s*$' } | Select-Object -Last $Lines) -join "`n"
}
foreach ($kind in 'stdout', 'stderr') {
    foreach ($f in Get-ChildItem -LiteralPath $dir -Filter "attempt-*.$kind.txt") {
        "--- $($f.Name) tail"
        Get-Content -LiteralPath $f.FullName -Tail 15
    }
}
