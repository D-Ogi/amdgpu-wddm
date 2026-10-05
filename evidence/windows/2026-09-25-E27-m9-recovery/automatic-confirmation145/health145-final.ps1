$ErrorActionPreference='Stop'
'now='+(Get-Date).ToString('s')
& C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe health read
if($LASTEXITCODE -ne 0){throw 'Health unavailable'}
& C:\BC250\m9\candidate07145\client\bc250kmd_cli.exe clock read
if($LASTEXITCODE -ne 0){throw 'Clock unavailable'}
Get-Process dwm,bc250mon | ForEach-Object { $_.ProcessName+' pid='+$_.Id+' start='+$_.StartTime.ToString('s') }
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
