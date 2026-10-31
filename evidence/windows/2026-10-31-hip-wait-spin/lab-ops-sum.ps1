# Lab side: the result of the last test-backend-ops run of one run directory. Counts of OK, FAIL and
# not-supported lines, the summary lines, and the first -First FAIL lines.
param([string]$Run = 'run-bload-socc', [int]$First = 12)
$p = Join-Path (Join-Path 'C:\BC250\tmp\t35hip' $Run) 'ops.out'
if (-not (Test-Path $p)) { "no $p"; exit 1 }
$all = Get-Content $p
$ok = @($all | Where-Object { $_ -match '\bOK\b' }).Count
$fail = @($all | Where-Object { $_ -match '\bFAIL\b' })
$ns = @($all | Where-Object { $_ -match 'not supported' }).Count
"ops.out $((Get-Item $p).LastWriteTime.ToString('s')) lines $($all.Count): OK $ok FAIL $($fail.Count) not-supported $ns"
$all | Where-Object { $_ -match 'tests passed|Backend .*:|Testing \d+ devices|backends passed|Failed|Skipping' } | Select-Object -Last 8
$fail | Select-Object -First $First
