$ErrorActionPreference='Stop'
if((Get-Process -Id $PID).SessionId -ne 0){throw 'SYSTEM session probe required'}
if((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 2).stop){throw 'Owner STOP'}
$text=& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1|Out-String
if($LASTEXITCODE -ne 0 -or $text -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)'){throw 'Temperature unavailable'}
$t=[double]$Matches[1]
if($t -ge 85){throw 'Temperature limit'}
@{temperature=$t;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json
