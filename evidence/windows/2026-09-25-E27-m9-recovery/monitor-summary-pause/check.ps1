$ErrorActionPreference='Stop'
$cli='C:\BC250\m9\candidate07146\client\bc250kmd_cli.exe'
$out='C:\BC250\mon\summary-pause-deploy'
if((Invoke-RestMethod http://127.0.0.1:2250/flags).stop){throw 'Owner STOP requested'}
[IO.File]::WriteAllText('C:\BC250\mon\graphics-summary.pause','M9 S4 isolation')
'pause_requested='+(Get-Date).ToString('o')
Start-Sleep -Seconds 6
& $cli log | Out-File "$out\paused-before.log"
& $cli health read
Start-Sleep -Seconds 12
& $cli log | Out-File "$out\paused-after.log"
& $cli health read
$state=Invoke-RestMethod http://127.0.0.1:2250/state
$state.panels | Where-Object name -eq graphics | ConvertTo-Json -Depth 8
'pause_interval_complete='+(Get-Date).ToString('o')
