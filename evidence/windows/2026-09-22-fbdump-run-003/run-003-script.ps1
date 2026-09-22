# fbdump run 001: EnableMmio only, device restart, info + dcn + fbdump, gate closed again.
$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\kmd\bc250kmd_cli.exe'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$gpu = Get-PnpDevice -Class Display | Where-Object { $_.FriendlyName -like 'BC-250*' } | Select-Object -First 1
"gpu $($gpu.InstanceId) $($gpu.Status)"
Set-ItemProperty $params -Name EnableMmioWrite -Value 0 -Type DWord
Set-ItemProperty $params -Name EnableMmio -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableVram -Value 1 -Type DWord
Set-ItemProperty $params -Name EnableVramWrite -Value 0 -Type DWord
"disable: " + ((pnputil /disable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' ')
Start-Sleep -Seconds 4
"enable:  " + ((pnputil /enable-device "$($gpu.InstanceId)" 2>&1 | Out-String).Trim() -replace '\s+', ' ')
Start-Sleep -Seconds 8
"== info"
& $cli info 2>&1
"== fbdump"
New-Item -ItemType Directory -Force C:\BC250\tmp | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
& $cli fbdump C:\BC250\tmp\scanout-003.bmp 2>&1
"fbdump took $($sw.ElapsedMilliseconds) ms"
Get-Item C:\BC250\tmp\scanout-003.bmp | Select-Object Length, LastWriteTime | Format-List | Out-String
Set-ItemProperty $params -Name EnableMmio -Value 0 -Type DWord
Set-ItemProperty $params -Name EnableVram -Value 0 -Type DWord
"EnableMmio back to 0"
