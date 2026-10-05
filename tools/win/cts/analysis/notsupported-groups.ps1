# NotSupported cases of one run grouped by test group (first N path components) and by the CTS's reason text.
param([Parameter(Mandatory)][string]$RunId, [int]$Depth = 4, [string]$Root = 'C:\BC250\cts')
$dir = Join-Path (Join-Path $Root 'results') $RunId
$rows = foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -Filter 'cases.tsv' -ErrorAction SilentlyContinue) { Import-Csv -LiteralPath $f.FullName -Delimiter "`t" }
$ns = @($rows | Where-Object { $_.status -eq 'NotSupported' })
"NotSupported $($ns.Count); columns: $((($rows | Select-Object -First 1).PSObject.Properties.Name) -join ',')"
$ns | Group-Object { ($_.case -split '\.')[0..($Depth - 1)] -join '.' } | Sort-Object Count -Descending | Select-Object -First 25 | ForEach-Object { "{0,6} {1}" -f $_.Count, $_.Name }
$reasonCol = (($rows | Select-Object -First 1).PSObject.Properties.Name | Where-Object { $_ -match 'detail|reason|message' } | Select-Object -First 1)
if ($reasonCol) { "-- by $reasonCol"; $ns | Group-Object { $_.$reasonCol } | Sort-Object Count -Descending | Select-Object -First 15 | ForEach-Object { "{0,6} {1}" -f $_.Count, $_.Name } }
