$ErrorActionPreference='Stop'
'now='+(Get-Date).ToString('s')
$stop=Invoke-RestMethod -Uri http://127.0.0.1:2250/flags -TimeoutSec 5
$stop | ConvertTo-Json -Compress
if($null -eq $stop.stop){throw 'STOP state missing'}
if($stop.stop){throw 'Owner STOP'}
$expected='36C298D454A75C73483C610802A6D7A6E8940FB973FDAF7B1CABFF93BC219021'
$cli='C:\BC250\m9\checked-confirm456\bc250kmd_cli.exe'
$actual=(Get-FileHash $cli -Algorithm SHA256).Hash
if($actual -ne $expected){throw 'CLI hash mismatch'}
'cli_sha256='+$actual
$r=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
if($r.UnconfirmedStarts -ne 0 -or $r.EnableFullWddm -ne 2){throw 'Unexpected existing guard state'}
& $cli confirm
if($LASTEXITCODE -ne 0){throw 'Checked confirmation failed'}
$r=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
'budget='+$r.UnconfirmedStarts
'policy='+$r.EnableFullWddm
'boot='+(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-Process dwm,bc250mon | ForEach-Object { $_.ProcessName+' pid='+$_.Id+' start='+$_.StartTime.ToString('s') }
