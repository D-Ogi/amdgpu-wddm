$ErrorActionPreference = 'Stop'
$cli = 'C:\BC250\tmp\m9-clock-check\bc250rd_cli.exe'
$expected = 'A056C897F9515BE08ADEE41C55F34B67CAE521922B9A2E2F9D2105A95879739D'
$hash = (Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash
'cli_sha256=' + $hash
if ($hash -ne $expected) { throw 'CLI hash mismatch' }
'positive clock-check 1000 820'
& $cli clock-check 1000 820
$good = $LASTEXITCODE
'positive_exit=' + $good
'negative read-only clock-check 1001 820'
& $cli clock-check 1001 820
$negative = $LASTEXITCODE
'negative_exit=' + $negative
'after negative: clock-check 1000 820'
& $cli clock-check 1000 820
$after = $LASTEXITCODE
'after_exit=' + $after
if ($good -ne 0 -or $negative -eq 0 -or $after -ne 0) { exit 1 }
'controls_passed'
