$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\e16-umd\bc250kmd_cli.exe'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gpu = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like 'BC-250*' } | Select-Object -First 1
"gpu $($gpu.InstanceId) $($gpu.Status)"
Set-ItemProperty $params -Name EnableMmioWrite -Value 0 -Type DWord
Set-ItemProperty $params -Name EnableMmio -Value 1 -Type DWord
"disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' ')
Start-Sleep -Seconds 4
"enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' ')
Start-Sleep -Seconds 8
& $cli info 2>&1 | Select-String 'version|last stage|gates|mode'
"== dcn dump 1"
& $cli dcn 2>&1
Start-Sleep -Seconds 1
"== dcn dump 2 (frame count should move)"
& $cli dcn 2>&1 | Select-String 'FRAME_COUNT|STATUS_POSITION|SURFACE_ADDRESS '
Set-ItemProperty $params -Name EnableMmio -Value 0 -Type DWord
"EnableMmio back to 0 (takes effect at the next device start; BAR5 stays mapped read-only until then)"
(Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like 'BC-250*' }).Status
