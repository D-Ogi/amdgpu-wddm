param([int]$Mhz)
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
'before DpmMaxMHz=' + (Get-ItemProperty $k).DpmMaxMHz
Set-ItemProperty $k -Name DpmMaxMHz -Value $Mhz -Type DWord
'after DpmMaxMHz=' + (Get-ItemProperty $k).DpmMaxMHz
'restart_utc=' + (Get-Date).ToUniversalTime().ToString('s')
shutdown.exe /r /t 5 /c "BC250 llama 1000 MHz comparison"
