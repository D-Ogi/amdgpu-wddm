# Read-only: dumps the KMD log ring (bc250kmd_cli log) to C:\BC250\tmp\kmdlog-<utc>.txt and prints its path,
# line count and the current QPC/UTC pair, so ring timestamps can be placed against receipts.
$ErrorActionPreference = 'Stop'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$out = "C:\BC250\tmp\kmdlog-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')).txt"
$qpc = [Diagnostics.Stopwatch]::GetTimestamp(); $utc = [DateTime]::UtcNow.ToString('o')
& $cli log | Set-Content -LiteralPath $out -Encoding ascii
"exit $LASTEXITCODE out $out lines $((Get-Content -LiteralPath $out).Count) qpc $qpc utc $utc"
