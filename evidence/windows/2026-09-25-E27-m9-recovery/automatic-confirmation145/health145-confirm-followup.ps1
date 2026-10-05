$ErrorActionPreference='Stop'
$cli='C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe'
'now='+(Get-Date).ToString('s')
$health=& $cli health read | Out-String
$health
if($LASTEXITCODE -ne 0 -or $health -notmatch 'flags=15 generation=29824235770 epoch=5'){throw 'Different or unconfirmed start'}
$r=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
'budget='+$r.UnconfirmedStarts
'policy='+$r.EnableFullWddm
if($r.UnconfirmedStarts -ne 0 -or $r.EnableFullWddm -ne 2){throw 'Unexpected guard state'}
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-Process dwm,bc250mon | ForEach-Object { $_.ProcessName+' pid='+$_.Id+' start='+$_.StartTime.ToString('s') }
Get-ChildItem C:\BC250\mon\log -Filter '*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object { Select-String -LiteralPath $_.FullName -Pattern 'full-WDDM start confirmed' | Select-Object -Last 3 | ForEach-Object {$_.Line} }
& $cli log
if($LASTEXITCODE -ne 0){throw 'Log unavailable'}
& $cli clock read
if($LASTEXITCODE -ne 0){throw 'Clock unavailable'}
