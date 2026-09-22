$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
Set-ItemProperty $params -Name EnableVramWrite -Value 1 -Type DWord
Set-ItemProperty $params -Name UnconfirmedStarts -Value 0 -Type DWord
$gpu = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like 'BC-250*' } | Select-Object -First 1
pnputil /disable-device "$($gpu.InstanceId)" | Out-Null
Start-Sleep -Seconds 3
pnputil /enable-device "$($gpu.InstanceId)" | Out-Null
Start-Sleep -Seconds 8
& 'C:\BC250\kmd\bc250kmd_cli.exe' info 2>&1 | Select-String 'version|gates'
