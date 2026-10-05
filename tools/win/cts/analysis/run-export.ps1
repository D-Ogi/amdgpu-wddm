# One TSV of a run's final result per case (the last attempt wins), for evidence: batch, case, status, attempt,
# duration_us, details. Prints totals by status over distinct cases. Hashtables, not Group-Object (19k rows).
param([Parameter(Mandatory)][string]$RunId, [Parameter(Mandatory)][string]$Out, [string]$Root = 'C:\BC250\cts')
$dir = Join-Path (Join-Path $Root 'results') $RunId
$final = @{}; $n = 0
foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -Filter 'cases.tsv' -ErrorAction SilentlyContinue) {
    foreach ($r in (Import-Csv -LiteralPath $f.FullName -Delimiter "`t")) {
        $n++
        $prev = $final[$r.case]
        if (!$prev -or [int]$r.attempt -ge [int]$prev.attempt) {
            $final[$r.case] = [pscustomobject]@{ batch = $f.Directory.Name; case = $r.case; status = $r.status; attempt = $r.attempt; duration_us = $r.duration_us; details = $r.details }
        }
    }
}
$sorted = $final.Values | Sort-Object batch, case
$sorted | Export-Csv -LiteralPath $Out -Delimiter "`t" -NoTypeInformation -Encoding UTF8
"rows $n, distinct cases $($final.Count)"
$t = @{}; foreach ($v in $final.Values) { $t[$v.status] = 1 + [int]$t[$v.status] }
$t.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { "{0,-14} {1}" -f $_.Key, $_.Value }
(Get-FileHash -LiteralPath $Out).Hash
