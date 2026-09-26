$ErrorActionPreference='Stop'
$out='C:\BC250\m9\candidate07145'
$cli=Join-Path $out 'client\bc250kmd_cli.exe'
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
'begin='+(Get-Date).ToString('s')
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
$seenPending=$false;$confirmed=$false;$generation=$null;$epoch=$null
for($i=0;$i -lt 27;$i++){
 $flags=Invoke-RestMethod -Uri http://127.0.0.1:2250/flags -TimeoutSec 5
 if($null -eq $flags.stop -or $flags.stop){throw 'Owner STOP or missing state'}
 $health=& $cli health read | Out-String
 if($LASTEXITCODE -ne 0){throw 'Health query failed'}
 if($health -notmatch 'flags=(\d+) generation=(\d+) epoch=(\d+) completed=(\d+) age_ms=(\d+) ready_ms=(\d+)'){throw 'Malformed health output'}
 $bits=[int]$Matches[1];$gen=$Matches[2];$ep=$Matches[3]
 if($null -eq $generation){$generation=$gen;$epoch=$ep}
 if($generation -ne $gen){throw 'Device restarted during acceptance'}
 $budget=(Get-ItemProperty $reg).UnconfirmedStarts
 if($budget -eq 1){$seenPending=$true}
 'sample='+(Get-Date).ToString('s')+' budget='+$budget+' '+$health.Trim()
 $state=Invoke-RestMethod -Uri http://127.0.0.1:2250/state -TimeoutSec 5
 $panel=@($state.panels | Where-Object name -eq kmd)
 $panel | ConvertTo-Json -Depth 8 -Compress
 if($budget -eq 0 -and ($bits -band 15) -eq 15){$confirmed=$true;break}
 if($budget -ne 0 -and $budget -ne 1){throw 'Unexpected start budget'}
 Start-Sleep -Seconds 5
}
if(-not $seenPending){throw 'Did not observe genuine pending start'}
if(-not $confirmed){throw 'Automatic confirmation did not complete'}
'auto_confirmation_verified'
Get-Process dwm,bc250mon | ForEach-Object { $_.ProcessName+' pid='+$_.Id+' start='+$_.StartTime.ToString('s') }
'boot_after='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
& $cli log
if($LASTEXITCODE -ne 0){throw 'Final log unavailable'}
