# Status totals of one run across all its batches (results\<run>\batch-NNNN\cases.tsv), plus every non-pass case.
param([Parameter(Mandatory)][string]$RunId, [string]$Root = 'C:\BC250\cts')
$dir = Join-Path (Join-Path $Root 'results') $RunId
$files = @(Get-ChildItem -LiteralPath $dir -Recurse -Filter 'cases.tsv' -ErrorAction SilentlyContinue)
$rows = foreach ($f in $files) { Import-Csv -LiteralPath $f.FullName -Delimiter "`t" | ForEach-Object { $_ | Add-Member -NotePropertyName batch -NotePropertyValue $f.Directory.Name -PassThru } }
"batches $($files.Count), cases $(@($rows).Count)"
$rows | Group-Object { $_.status } | Sort-Object Count -Descending | ForEach-Object { "{0,-22} {1}" -f $_.Name, $_.Count }
"-- non-pass (excluding NotSupported and warnings)"
$rows | Where-Object { $_.status -notin @('Pass', 'NotSupported', 'QualityWarning', 'CompatibilityWarning') } | ForEach-Object { "$($_.batch) $($_.status) $($_.case)" }
