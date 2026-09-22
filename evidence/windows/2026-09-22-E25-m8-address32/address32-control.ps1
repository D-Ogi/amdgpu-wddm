$ErrorActionPreference='Stop'
$cli='C:\BC250\m8\bc250kmd_cli.exe'
$raw=& 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 2>&1 | Out-String
$raw
if ($raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'temperature unavailable' }
if ([double]$Matches[1] -ge 85) { throw 'temperature limit' }
& $cli fence gfx 1 ib
if ($LASTEXITCODE -ne 0) { throw 'positive control failed' }
