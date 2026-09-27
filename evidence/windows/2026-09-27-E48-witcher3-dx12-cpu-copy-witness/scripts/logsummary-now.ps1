# Read-only: current KMD log summary counters (paging operations, allocations) after run 010.
$ErrorActionPreference = 'Stop'
$cli = 'C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe'
$out = 'C:\BC250\m12\witcher3-dx12\log-summary-after-010.txt'
& $cli log summary *> $out
Get-Content $out | Where-Object { $_ -match 'paging operation|alloc \d+/|opened/closed|node 1|umd:|virtual, ' } | Select-Object -Last 12
"utc=" + [DateTime]::UtcNow.ToString('o')
