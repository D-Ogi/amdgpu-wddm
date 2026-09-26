$ErrorActionPreference = 'Stop'
$state = Invoke-RestMethod http://127.0.0.1:2250/state
if ($state.stop) { throw 'Owner STOP requested' }
$old = 'C:\BC250\mon\bc250mon.exe'
$new = 'C:\BC250\mon\m13\bc250mon.exe'
$backup = 'C:\BC250\mon\bc250mon-pre-m13.exe'
if (!(Test-Path $backup)) { Copy-Item -LiteralPath $old -Destination $backup }
Get-Process bc250mon | Stop-Process -Force
Copy-Item -LiteralPath $new -Destination $old -Force
Get-FileHash -Algorithm SHA256 $old | Select-Object Hash
schtasks /run /tn 'BC250 monitor overlay'
if ($LASTEXITCODE -ne 0) { throw 'Overlay task did not start' }
Start-Sleep -Seconds 12
$s = Invoke-RestMethod http://127.0.0.1:2250/state
$s.panels | Where-Object { $_.name -eq 'graphics' } | ConvertTo-Json -Depth 8
'BOOT=' + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
